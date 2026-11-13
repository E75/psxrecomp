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
 *   block leader uses: EPC is the guest $ra, the handler runs, and a
 *   same-thread resume restores the interrupted GPRs. Returns 1 when guest time
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

uint32_t we_yield_until_vblank(struct CPUState* cpu);
int we_wait_guest_word(struct CPUState* cpu, uint32_t addr, uint32_t mask,
                       uint32_t want, uint32_t max_vblanks);

#ifdef __cplusplus
}
#endif

#endif
