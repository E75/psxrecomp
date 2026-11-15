/* runtime/tests/test_yield_continuation.c — offline proof of the C-body yield
 * host-continuation contract (docs/YIELD_CONTINUATION.md).
 *
 * Build + run (from the repo root):
 *   clang -std=gnu89 -Wall -Wextra -Wno-unused-parameter \
 *         -Iruntime/include runtime/tests/test_yield_continuation.c \
 *         -o build/test_yield_continuation.exe
 *   ./build/test_yield_continuation.exe
 *
 * What is real here: psx_guest_yield.c itself (included verbatim below), the
 * flag, the depth counter, the cycle credit, the trace, the stats, the
 * g_dirty_safe_resume_pc withholding and the g_psx_host_continuation handshake.
 *
 * What is modelled: the interrupt DELIVERY. Linking runtime/src/interrupts.c
 * needs the GPU, CD, DMA, fibers and the whole scheduler, so the three
 * decisions the contract depends on are reproduced from the real code, with the
 * line numbers quoted:
 *   - the EPC selection (interrupts.c 1678-1724): a plausible guest resume PC
 *     becomes the architectural EPC; no resume PC installs the legacy host
 *     sentinel PSX_EXC_SENTINEL_PC and marks the delivery LEGACY_SENTINEL;
 *   - the ISR return (traps.c 1173-1176): an RFE/jr to that sentinel while
 *     in_exception aborts the dispatch with cpu->pc = 0 and longjmps back;
 *   - the epilogue (interrupts.c 2044-2053, 2131-2135): a LEGACY_SENTINEL or
 *     same-thread resume restores the interrupted GPRs/HI/LO.
 * Everything else in the delivery is irrelevant to the contract: the harness
 * asserts what the C body observes, not how the handler got there. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycle_freeze.h"
#include "psx_cycles.h"
#include "psx_guest_yield.h"

/* ---- host globals the inlined psx_advance_cycles() touches ---------------- */
uint64_t  psx_cycle_count;
uint64_t  psx_next_service_cycle;
int       psx_in_device_service;
int       g_event_step_conservative;
int       g_ls_replay_active;
uint32_t  g_psx_cyc_batch;
uint32_t  g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
uint32_t  g_psx_gcs_recip_q16 = 65536u;   /* scale 1: PSX_GCS_ACTIVE() is false */
int       g_psx_cyc_bb_defer;

/* ---- runtime globals the yield and the delivery model touch --------------- */
int      g_psx_guest_time_frozen;
uint32_t g_dirty_safe_resume_pc;
uint64_t g_irq_deliver_count;
extern int g_psx_host_continuation;       /* psx_guest_yield.c */
int g_psx_guest_call_depth;               /* mod_guest_services.c */

/* ---- the fake machine ---------------------------------------------------- */
#define PSX_TEST_RAM_BYTES 0x200000u
#define PSX_TEST_VBLANK_PERIOD 564480u      /* NTSC 33.8688 MHz / 60 */

static struct CPUState s_cpu;
static uint64_t s_next_vblank;
static uint32_t s_i_stat, s_i_mask;
static int      s_in_exception;
static uint32_t s_watch_addr;
static uint32_t s_watch_word;

static int      s_deliver_calls;      /* entries into the delivery */
static uint32_t s_candidate;          /* resume PC the delivery evaluated */
static uint32_t s_epc_installed;      /* EPC the delivery installed */
static int      s_last_legacy;        /* delivery took the legacy sentinel */
static int      s_isr_runs;
static uint32_t s_isr_exit_pc;        /* where the ISR's RFE left cpu->pc */
static int      s_isr_probe;          /* make the ISR call the yield */
static int      s_isr_probe_ret;
static int      s_reenter_probe;      /* make the delivery call the yield first */
static int      s_reenter_probe_ret;

uint32_t psx_read_word(uint32_t addr)
{
    return addr == s_watch_addr ? s_watch_word : 0u;
}

uint64_t psx_get_cycle_count(void)
{
    return psx_cycle_count + (uint64_t)g_psx_cyc_batch;
}

void psx_devices_service_to_now(void)
{
    /* Device servicing: each VBlank edge crossed raises I_STAT.VBLANK. */
    while (s_next_vblank <= psx_cycle_count) {
        s_i_stat |= 1u << IRQ_VBLANK;
        s_next_vblank += PSX_TEST_VBLANK_PERIOD;
    }
}

void psx_advance_cycles_slow(uint32_t cycles)
{
    psx_cycle_count += (uint64_t)cycles;
    psx_devices_service_to_now();
}

uint32_t interrupts_cycles_to_vblank(void)
{
    return (uint32_t)(s_next_vblank - psx_cycle_count);
}

int psx_get_in_exception(void) { return s_in_exception; }

/* Trivial guest ISR: a few instructions that RFE, plus the guest-visible
 * effects the C body is waiting for. */
static void fake_isr(struct CPUState *cpu)
{
    s_isr_runs++;
    cpu->gpr[29] -= 32u;                 /* pushes its own frame */
    cpu->gpr[31] = 0xBFC10B0Cu;          /* clobbers $ra like real handler code */
    cpu->gpr[2]  = 1u;                   /* v0 = 1 (DAT_8005ff02 shape) */
    s_watch_word = 0u;                   /* the ack the C body spins on */
    s_i_stat = 0u;                       /* handler acked I_STAT */
    cpu->pc = s_epc_installed;           /* ReturnFromException: jr k0 (+ rfe) */
    s_isr_exit_pc = cpu->pc;
    if (s_isr_probe) {
        s_isr_probe = 0;
        s_isr_probe_ret = (int)we_yield_until_vblank(cpu);
    }
}

/* One delivery, modelled on interrupts.c 1678-1724 + traps.c 1173-1176 +
 * interrupts.c 2044-2053/2131-2135. `candidate` is the resume PC the delivery
 * is allowed to use as EPC (0 = none). */
static void test_deliver(struct CPUState *cpu, uint32_t candidate)
{
    uint32_t saved_gpr[32];
    uint32_t saved_hi, saved_lo;
    int i, legacy, prev_in_exception;

    s_candidate = candidate;

    if (s_reenter_probe) {
        s_reenter_probe = 0;
        s_reenter_probe_ret = (int)we_yield_until_vblank(cpu);
    }
    if ((s_i_stat & s_i_mask) == 0u) return;   /* nothing deliverable */

    if (candidate != 0u && (candidate & 3u) == 0u &&
        ((candidate & 0x1FFFFFFFu) < PSX_TEST_RAM_BYTES ||
         ((candidate & 0x1FFFFFFFu) >= 0x1FC00000u &&
          (candidate & 0x1FFFFFFFu) < 0x1FC80000u))) {
        s_epc_installed = candidate;   /* real EPC: guest code resumes there */
        legacy = 0;
    } else {
        s_epc_installed = PSX_EXC_SENTINEL_PC;   /* host stop token */
        legacy = 1;
    }
    s_last_legacy = legacy;
    g_irq_deliver_count++;

    for (i = 0; i < 32; i++) saved_gpr[i] = cpu->gpr[i];
    saved_hi = cpu->hi;
    saved_lo = cpu->lo;

    prev_in_exception = s_in_exception;
    s_in_exception = 1;
    fake_isr(cpu);
    s_in_exception = prev_in_exception;

    /* psx_unknown_dispatch: RFE to the host sentinel inside the window. */
    if (cpu->pc == PSX_EXC_SENTINEL_PC) cpu->pc = 0u;

    if (legacy ||
        (s_epc_installed != 0u && cpu->pc == s_epc_installed)) {
        for (i = 0; i < 32; i++) cpu->gpr[i] = saved_gpr[i];
        cpu->hi = saved_hi;
        cpu->lo = saved_lo;
    }
}

void psx_check_interrupts_at(struct CPUState *cpu, uint32_t resume_pc)
{
    int host_continuation;

    s_deliver_calls++;

    /* interrupts.c 2311-2331: a host call's return boundary substitutes the
     * caller's continuation for a non-plausible resume PC, unless this is a
     * C-body yield (the flag is consumed on read). */
    host_continuation = g_psx_host_continuation;
    g_psx_host_continuation = 0;
    if (!host_continuation && g_psx_guest_call_depth != 0 &&
        !(resume_pc != 0u && (resume_pc & 3u) == 0u &&
          (resume_pc & 0x1FFFFFFFu) < PSX_TEST_RAM_BYTES)) {
        resume_pc = 0x80100000u;   /* stand-in for the call's continuation */
    }
    test_deliver(cpu, resume_pc);
}

void psx_check_interrupts(struct CPUState *cpu)
{
    s_deliver_calls++;
    /* The legacy entry: no compiled resume PC; the dirty latch or nothing. */
    test_deliver(cpu, g_dirty_safe_resume_pc);
}

/* psx_guest_yield.c, verbatim. It needs the globals above, so it comes last
 * among the machine code; the tests below may then reach its statics. */
#include "../src/psx_guest_yield.c"

/* ---- assertions ---------------------------------------------------------- */
static int s_failures;
static int s_checks;

#define CHECK(cond) do { \
        s_checks++; \
        if (!(cond)) { \
            s_failures++; \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static void machine_init(uint32_t pc, uint32_t ra, uint32_t sp,
                         uint32_t watch_word, uint32_t i_stat, uint32_t i_mask)
{
    memset(&s_cpu, 0, sizeof(s_cpu));
    s_cpu.pc = pc;
    s_cpu.gpr[31] = ra;
    s_cpu.gpr[29] = sp;
    s_cpu.gpr[2] = 0u;
    s_watch_addr = 0x8005FD5Cu;      /* DAT_8005fd5c shape */
    s_watch_word = watch_word;
    psx_cycle_count = 0;
    g_psx_cyc_batch = 0;
    psx_next_service_cycle = 0;      /* service on every advance */
    psx_in_device_service = 0;
    g_ls_replay_active = 0;
    g_event_step_conservative = 0;
    g_psx_guest_time_frozen = 0;
    g_dirty_safe_resume_pc = 0;
    g_psx_host_continuation = 0;
    g_irq_deliver_count = 0;
    s_next_vblank = PSX_TEST_VBLANK_PERIOD;
    s_i_stat = i_stat;
    s_i_mask = i_mask;
    s_in_exception = 0;
    s_deliver_calls = 0;
    s_candidate = 0xDEADBEEFu;
    s_epc_installed = 0xDEADBEEFu;
    s_last_legacy = -1;
    s_isr_runs = 0;
    s_isr_exit_pc = 0xDEADBEEFu;
    s_isr_probe = 0;
    s_isr_probe_ret = -1;
    s_reenter_probe = 0;
    s_reenter_probe_ret = -1;
    s_yield_depth = 0;
}

static void test_default_off(void)
{
    printf("T0  default is off (env unset, no override)\n");
    s_c_body_yield = -1;             /* re-latch from the environment */
#if defined(_WIN32)
    _putenv_s("PSX_C_BODY_YIELD", "");
#else
    unsetenv("PSX_C_BODY_YIELD");
#endif
    CHECK(we_yield_c_body_yield_enabled() == 0);
#if defined(_WIN32)
    _putenv_s("PSX_C_BODY_YIELD", "1");
    s_c_body_yield = -1;
    CHECK(we_yield_c_body_yield_enabled() == 1);
    _putenv_s("PSX_C_BODY_YIELD", "");
    s_c_body_yield = -1;
#endif
    CHECK(we_yield_c_body_yield_enabled() == 0);
    we_yield_set_c_body_yield(0);
    CHECK(we_yield_c_body_yield_enabled() == 0);
}

static void test_sentinel_delivery_returns(void)
{
    WeYieldStats st;
    uint64_t before;
    uint32_t to_edge;

    printf("T1  C-body yield: sentinel delivery runs the ISR once and returns\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    to_edge = interrupts_cycles_to_vblank();
    CHECK(to_edge == PSX_TEST_VBLANK_PERIOD);
    before = psx_get_cycle_count();

    CHECK(we_yield_until_vblank(&s_cpu) == 1);

    CHECK(s_deliver_calls == 1);                 /* one delivery entry */
    CHECK(s_candidate == 0u);                    /* no guest resume PC handed over */
    CHECK(s_last_legacy == 1);                   /* legacy host-sentinel delivery */
    CHECK(s_epc_installed == PSX_EXC_SENTINEL_PC);
    CHECK(s_isr_runs == 1);                      /* the ISR ran exactly once */
    CHECK(s_isr_exit_pc == PSX_EXC_SENTINEL_PC); /* its RFE stopped at the host */
    CHECK(s_cpu.pc == 0u);                       /* the caller's own value */
    CHECK(s_cpu.gpr[31] == 0x8004C288u);         /* $ra restored */
    CHECK(s_cpu.gpr[29] == 0x801FFEE0u);         /* $sp restored */
    CHECK(s_cpu.gpr[2] == 0u);                   /* the ISR's v0=1 undone */
    CHECK(s_watch_word == 0u);                   /* the word the spin waited on */
    CHECK(psx_get_cycle_count() - before == to_edge);  /* credited once, no more */
    CHECK(s_yield_depth == 0);
    we_yield_stats(&st);
    CHECK(st.yields == 1);
    CHECK(st.deliveries == 1);
    CHECK(st.pc_restored == 0);   /* cpu->pc was already 0: nothing to put back */
}

static void test_legacy_ra_is_the_wedge(void)
{
    printf("T2  flag off: the stale $ra becomes EPC and the C body loses control\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(0);

    CHECK(we_yield_until_vblank(&s_cpu) == 1);

    CHECK(s_deliver_calls == 1);
    CHECK(s_candidate == 0x8004C288u);            /* the stale $ra */
    CHECK(s_last_legacy == 0);
    CHECK(s_epc_installed == 0x8004C288u);        /* mid-function guest code */
    CHECK(s_isr_runs == 1);
    CHECK(s_cpu.pc == 0x8004C288u);               /* guest resumes there, not C */
    CHECK(s_watch_word == 0u);
    we_yield_set_c_body_yield(1);
}

static void test_second_yield(void)
{
    printf("T3  a second yield after the return still delivers and returns\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);

    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(s_isr_runs == 1);
    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(s_isr_runs == 2);                       /* the next edge raised it again */
    CHECK(s_deliver_calls == 2);
    CHECK(s_cpu.pc == 0u);
    CHECK(s_cpu.gpr[31] == 0x8004C288u);
}

static void test_nested_yield(void)
{
    WeYieldStats before, after;

    printf("T4  a nested yield (re-entered from the delivery) returns 0\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    we_yield_stats(&before);
    s_reenter_probe = 1;

    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(s_reenter_probe_ret == 0);              /* refused, did not recurse */
    CHECK(s_isr_runs == 1);                       /* exactly one ISR */
    CHECK(s_deliver_calls == 1);
    we_yield_stats(&after);
    CHECK(after.refused_nested == before.refused_nested + 1u);
    CHECK(after.deliveries == before.deliveries + 1u);

    printf("T5  a yield from inside the ISR returns 0 as well\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    s_isr_probe = 1;
    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(s_isr_probe_ret == 0);
    CHECK(s_isr_runs == 1);
    CHECK(s_cpu.pc == 0u);

    printf("T6  the depth counter is clear again after every return\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    s_yield_depth = 1;                            /* forced re-entry */
    CHECK(we_yield_until_vblank(&s_cpu) == 0);
    CHECK(s_deliver_calls == 0);
    s_yield_depth = 0;
    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(s_isr_runs == 1);
}

static void test_no_delivery(void)
{
    uint64_t before;
    uint32_t to_edge;

    printf("T7  nothing unmasked: cycles credited once, no ISR, pc untouched\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK, 0u);
    we_yield_set_c_body_yield(1);
    to_edge = interrupts_cycles_to_vblank();
    before = psx_get_cycle_count();

    CHECK(we_yield_until_vblank(&s_cpu) == 1);
    CHECK(psx_get_cycle_count() - before == to_edge);
    CHECK(s_isr_runs == 0);
    CHECK(s_cpu.pc == 0u);
    CHECK(s_cpu.gpr[31] == 0x8004C288u);
    CHECK(g_irq_deliver_count == 0u);
}

static void test_refusals(void)
{
    WeYieldStats st;

    printf("T8  frozen guest time and in-exception refuse without advancing\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);

    g_psx_guest_time_frozen = 1;
    CHECK(we_yield_until_vblank(&s_cpu) == 0);
    CHECK(psx_get_cycle_count() == 0u);
    CHECK(s_deliver_calls == 0);
    g_psx_guest_time_frozen = 0;

    s_in_exception = 1;
    CHECK(we_yield_until_vblank(&s_cpu) == 0);
    CHECK(psx_get_cycle_count() == 0u);
    s_in_exception = 0;

    we_yield_stats(&st);
    CHECK(st.refused_frozen == 1);
    CHECK(st.refused_exception == 1);
}

static void test_wait_word(void)
{
    printf("T9  we_wait_guest_word: the ISR clears the word, the helper returns 1\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    CHECK(we_wait_guest_word(&s_cpu, s_watch_addr, 0xFFFFu, 0u, 3u) == 1);
    CHECK(s_isr_runs == 1);

    printf("T10 we_wait_guest_word: an unset word times out after max_vblanks\n");
    machine_init(0u, 0x8004C288u, 0x801FFEE0u, 1u, 1u << IRQ_VBLANK,
                 1u << IRQ_VBLANK);
    we_yield_set_c_body_yield(1);
    CHECK(we_wait_guest_word(&s_cpu, 0x8005FD60u, 0xFFFFu, 2u, 2u) == 0);
    CHECK(s_isr_runs == 2);
}

static void test_sentinel_distinct(void)
{
    printf("T11 the host stop token is not the host-call sentinel\n");
    CHECK(PSX_EXC_SENTINEL_PC != 0x8F000000u);
    CHECK((PSX_EXC_SENTINEL_PC & 0x1FFFFFFFu) < PSX_TEST_RAM_BYTES);
    /* The delivery never accepts 0x8F000000 as EPC either: it is not RAM and
     * not ROM, so it would also select the host stop token. */
    CHECK(!(((0x8F000000u & 0x1FFFFFFFu) < PSX_TEST_RAM_BYTES) ||
            ((0x8F000000u & 0x1FFFFFFFu) >= 0x1FC00000u &&
             (0x8F000000u & 0x1FFFFFFFu) < 0x1FC80000u)));
}

int main(void)
{
    printf("test_yield_continuation: C-body yield host-continuation contract\n");

    test_default_off();
    test_sentinel_delivery_returns();
    test_legacy_ra_is_the_wedge();
    test_second_yield();
    test_nested_yield();
    test_no_delivery();
    test_refusals();
    test_wait_word();
    test_sentinel_distinct();

    printf("%d checks, %d failure(s)\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
