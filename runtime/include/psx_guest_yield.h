#ifndef PSXRECOMP_PSX_GUEST_YIELD_H
#define PSXRECOMP_PSX_GUEST_YIELD_H

/* Cooperative VBlank yield for decompiled C (docs/BOOT_BLOCKERS.md).
 *
 * A decompiled body runs as host C, so a `while (flip_request) ;` spin inside it
 * can never finish: only the guest VBlank handler clears that word, and the
 * handler only runs when the runtime delivers the interrupt, which happens at a
 * guest instruction boundary the C body does not have. These two calls give the
 * body a boundary it can hand control back to.
 *
 * we_yield_until_vblank(cpu)
 *   Advances guest time to the next VBlank edge (device servicing included, so
 *   every timer/CD/DMA deadline in between is met exactly as if the guest had
 *   spun), then delivers the pending interrupt through the same path a guest
 *   block leader uses and returns to the C body. Returns 1 when guest time
 *   advanced, 0 when it refused (no CPU, frozen guest time, already inside an
 *   exception, or re-entered from a handler).
 *
 *   Preserved: every GPR the handler does not architecturally change (a
 *   same-thread delivery restores them), HI/LO, COP0, GTE and the guest RAM
 *   the C body owns. The caller's own C locals are untouched; the guest stack
 *   is not modified. What may run: device servicing, the VBlank IRQ delivery
 *   and the guest ISR (which may itself call guest functions or change thread).
 *
 *   The caller re-checks its guest condition after the call; a helper that
 *   returned 0 must not be retried in a tight loop (nothing can progress).
 *
 * Host-continuation contract (docs/YIELD_CONTINUATION.md)
 *   A dispatcher-entered C body has no guest resume point, and the delivery has
 *   no "run until pc" semantics to satisfy: the value handed to
 *   psx_check_interrupts_at() is only a CANDIDATE EPC. With
 *   PSX_C_BODY_YIELD=1 (or we_yield_set_c_body_yield(1), default off) the yield
 *   hands the delivery NO guest resume PC instead of the C frame's stale $ra,
 *   so the delivery selects the legacy host sentinel PSX_EXC_SENTINEL_PC
 *   (0x80000048) as EPC: the ISR's RFE stops at the host instead of entering
 *   guest code, the epilogue restores the interrupted GPRs and returns, and the
 *   yield restores cpu->pc to the value the caller had. Without the flag the
 *   historical path (stale $ra as EPC) is kept bit-for-bit, so the boot can A/B
 *   the two.
 *
 *   Cost when on: one delivery per yield, exactly the cycles the advance
 *   charged (the same amount a real spin would have consumed to the edge), and
 *   no guest-visible difference beyond the ISR the spin was waiting for. Not
 *   resumable: if the yielded ISR switches threads the scheduler escapes and
 *   the C frame is abandoned (the trace line prints "enter" without "return").
 *
 * we_wait_guest_word(cpu, addr, mask, want, max_vblanks)
 *   The whole loop, bounded: yield until (word & mask) == want, at most
 *   max_vblanks VBlank edges (0 = check once without yielding). Returns 1 when
 *   the condition holds, 0 on timeout or refusal. This is the form the frame
 *   loop should call where the original code spun on a flip-request word.
 *
 * Guest-time semantics match the retired 0x80044BBC hook: the same clock
 * advance, the same VBlank counter value and the same RCNT value on exit, so
 * match time, savestates and netplay digests are unchanged. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* A/B switch for the host-continuation contract. PSX_C_BODY_YIELD=1 (or any
 * value not starting with '0') turns it on; unset/0 keeps the historical
 * behaviour. Latched on first query; we_yield_set_c_body_yield() overrides it
 * (game.toml [runtime] c_body_yield, wired by the host). */
int  we_yield_c_body_yield_enabled(void);
void we_yield_set_c_body_yield(int on);

/* Counters for the boot A/B (docs/YIELD_CONTINUATION.md). */
typedef struct WeYieldStats {
    uint64_t yields;             /* calls that advanced guest time */
    uint64_t deliveries;         /* hardware deliveries observed during yields */
    uint64_t legacy_resume;      /* plausible $ra handed over as EPC (flag off) */
    uint64_t pc_restored;        /* cpu->pc put back to the C body's value */
    uint64_t refused_no_cpu;
    uint64_t refused_nested;
    uint64_t refused_frozen;
    uint64_t refused_exception;
} WeYieldStats;
void we_yield_stats(WeYieldStats *out);

uint32_t we_yield_until_vblank(struct CPUState* cpu);
int we_wait_guest_word(struct CPUState* cpu, uint32_t addr, uint32_t mask,
                       uint32_t want, uint32_t max_vblanks);

#ifdef __cplusplus
}
#endif

#endif
