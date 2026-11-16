/* psx_guest_yield.c — see psx_guest_yield.h.
 *
 * The runtime owns guest time (psx_cycles.c + the exact VBlank clock), so a
 * wait that has no guest instruction boundary to return to is served by
 * advancing to the next edge and delivering the interrupt there. That is the
 * same sequence psx_check_interrupts() performs for a compiled block leader,
 * which is why the guest sees the identical VBlank/RCNT values a real spin
 * would have produced.
 *
 * The delivery needs a STOP CONDITION, not a guest resume point. Nothing in the
 * interrupt path runs the guest "until it reaches pc": the value handed to
 * psx_check_interrupts_at() is only a CANDIDATE EPC (interrupts.c 1678-1724).
 * A plausible guest PC becomes the architectural EPC, the ISR RFEs to it and
 * the guest continues there - the host C frame is abandoned. No guest resume PC
 * selects the legacy host sentinel PSX_EXC_SENTINEL_PC (0x80000048), whose
 * contract is exactly "the ISR returned to the host: restore the interrupted
 * GPRs and hand control back to the live host chain" (traps.c 1173-1176 ->
 * interrupts.c 2131-2135). That is what a dispatcher-entered C body needs.
 *
 * Measured failure this replaces: the body's $ra is a stale guest PC
 * (pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 depth=0), so the old code
 * installed it as EPC, the ISR RFE'd into mid-function guest code and the C
 * body never continued (docs/BOOT_BLOCKERS.md 9.9: 128 Pause retries,
 * 1.3 fps against stock's 34 fps).
 */

#include "psx_guest_yield.h"

#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycle_freeze.h"
#include "psx_cycles.h"
#include "psx_guest_call.h"

extern uint32_t psx_read_word(uint32_t addr);
/* dirty_ram_interp.c: the interpreter's committed resume PC. It takes priority
 * over the compiled latch in the delivery's EPC selection, so a C-body yield
 * withholds it for the delivery and puts it back afterwards. */
extern uint32_t g_dirty_safe_resume_pc;
/* interrupts.c: monotonic count of hardware deliveries. Read-only here; the
 * yield reports the delta so "exactly one delivery per yield" is measurable. */
extern uint64_t g_irq_deliver_count;

/* Guest code lives in RAM (0x80010000..) and the BIOS (0xA0000000..). Anything
 * else - notably the 0x8F000000 sentinel a host-initiated call uses as $ra - is
 * not a usable exception return address. */
static int guest_resume_pc(uint32_t pc)
{
    return pc >= 0x1000u && pc < 0x80200000u && (pc & 3u) == 0u;
}

/* Consumed by the next psx_check_interrupts_at() (interrupts.c): a C-body
 * delivery must not be mistaken for a host call's return boundary, whose
 * resume substitution would hand the delivery a guest PC to resume at. Read at
 * the very top of _at, before anything can longjmp, so it cannot leak. */
int g_psx_host_continuation = 0;

static int s_yield_depth;
static WeYieldStats s_stats;
static uint64_t s_yield_seq;

static int s_c_body_yield = -1;   /* -1 = not latched yet */

int we_yield_c_body_yield_enabled(void)
{
    if (s_c_body_yield < 0) {
        const char *e = getenv("PSX_C_BODY_YIELD");
        s_c_body_yield = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_c_body_yield;
}

void we_yield_set_c_body_yield(int on)
{
    s_c_body_yield = on ? 1 : 0;
}

void we_yield_stats(WeYieldStats *out)
{
    if (out) *out = s_stats;
}

/* First few calls, then a sparse heartbeat: "enter" without a matching
 * "return" at the same # means the delivery did not come back to the C body
 * (the yielded ISR switched threads and the scheduler escaped). */
static void yield_trace(const char *what, const struct CPUState *cpu)
{
    if (s_yield_seq > 4u && (s_yield_seq % 1024u) != 0u) return;
    fprintf(stderr,
            "we_yield: %s #%llu pc=0x%08X ra=0x%08X sp=0x%08X call_depth=%d\n",
            what, (unsigned long long)s_yield_seq, cpu->pc, cpu->gpr[31],
            cpu->gpr[29], g_psx_guest_call_depth);
    fflush(stderr);
}

uint32_t we_yield_until_vblank(struct CPUState* cpu)
{
    if (!cpu) { s_stats.refused_no_cpu++; return 0; }
    if (s_yield_depth) { s_stats.refused_nested++; return 0; }
    if (g_psx_guest_time_frozen) { s_stats.refused_frozen++; return 0; }
    if (psx_get_in_exception()) { s_stats.refused_exception++; return 0; }

    s_yield_depth = 1;
    s_stats.yields++;
    s_yield_seq++;
    const uint64_t before = psx_get_cycle_count();
    uint32_t to_edge = interrupts_cycles_to_vblank();
    if (to_edge == 0u) to_edge = 1u;           /* already at the edge: cross it */
    psx_advance_cycles(to_edge);

    const uint32_t entry_pc = cpu->pc;
    yield_trace("enter", cpu);
    psx_sr_trace("yield_entry", cpu);

    /* Deliver what the advance raised. The VBlank handler is what clears the
     * game's flip-request word, so this is the step the C spin was missing. */
    const uint64_t deliv_before = g_irq_deliver_count;
    if (we_yield_c_body_yield_enabled()) {
        /* Host-continuation contract: hand the delivery NO guest resume PC, so
         * it selects the legacy host sentinel as EPC and stops at the host
         * instead of entering guest code. The epilogue then restores every GPR
         * ($ra/$sp/$v0 come back as the C body left them). A C body has no
         * guest PC of its own, so cpu->pc is put back to the value the caller
         * had - the live host chain IS the continuation. */
        const uint32_t saved_dirty_pc = g_dirty_safe_resume_pc;
        /* Depth/state latches an escape would leave raised. The sentinel exit
         * longjmps out of the handler, which skips psx_mod_call_guest_ex()'s
         * epilogue (mod_guest_services.c 220-221): the guest call frame and the
         * snapshot-safety latch it raised there are never unwound. Measured on
         * the live boot: depth grew by one across every yield (#2 2->3, #3
         * 4->5, #4 7->8) until later calls were mis-nested and the boot wedged.
         * psx_check_interrupts already restores its own g_psx_dispatch_depth for
         * this exact reason (interrupts.c 1885-1891); these two are ours. The
         * abandoned s_call_resume[]/s_sentinel[] slots above the restored depth
         * are safe: every index < depth is rewritten by the next enter. */
        const int saved_call_depth = g_psx_guest_call_depth;
        const unsigned saved_snap_depth = psx_snapshot_host_call_depth();

        g_dirty_safe_resume_pc = 0;
        psx_sr_trace("before_deliver", cpu);
        g_psx_host_continuation = 1;
        psx_check_interrupts_at(cpu, 0u);
        g_psx_host_continuation = 0;
        g_dirty_safe_resume_pc = saved_dirty_pc;
        psx_sr_trace("after_deliver", cpu);

        /* Invariant: depth out == depth in, whether the delivery ran a nested
         * ISR, refused, or found nothing pending. */
        if (g_psx_guest_call_depth != saved_call_depth) {
            g_psx_guest_call_depth = saved_call_depth;
            s_stats.call_depth_restored++;
        }
        if (psx_snapshot_host_call_depth() != saved_snap_depth) {
            psx_snapshot_host_call_depth_set(saved_snap_depth);
            s_stats.snapshot_depth_restored++;
        }
        if (cpu->pc != entry_pc) {
            cpu->pc = entry_pc;
            s_stats.pc_restored++;
        }
    } else {
        const uint32_t resume = cpu->gpr[31];
        if (guest_resume_pc(resume)) {
            psx_check_interrupts_at(cpu, resume);
            s_stats.legacy_resume++;
        } else {
            psx_check_interrupts(cpu);
        }
    }
    s_stats.deliveries += g_irq_deliver_count - deliv_before;

    yield_trace("return", cpu);
    s_yield_depth = 0;
    return psx_get_cycle_count() != before ? 1u : 0u;
}

int we_wait_guest_word(struct CPUState* cpu, uint32_t addr, uint32_t mask,
                       uint32_t want, uint32_t max_vblanks)
{
    if ((psx_read_word(addr) & mask) == want) return 1;
    for (uint32_t i = 0; i < max_vblanks; i++) {
        if (!we_yield_until_vblank(cpu)) return 0;
        if ((psx_read_word(addr) & mask) == want) return 1;
    }
    return 0;
}
