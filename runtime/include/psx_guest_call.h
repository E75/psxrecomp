#ifndef PSXRECOMP_PSX_GUEST_CALL_H
#define PSXRECOMP_PSX_GUEST_CALL_H

/* Host-initiated guest calls and the guest's "resume PC".
 *
 * psx_mod_call_guest() enters guest code from host C. The callee returns to
 * the caller-supplied return address (the hybrid layer uses 0x8F000000), which
 * is a sentinel, NOT a code address: the caller is host C, and the guest's real
 * continuation is the C frame's $ra (the address the opt-in function publishes
 * when it finishes).
 *
 * Everything that PERSISTS "the guest PC this thread will resume at" must use
 * that real continuation, never the sentinel:
 *
 *   - a thread switch inside the call (ChangeThread/WaitEvent) saves the
 *     yielding thread's context to its TCB (traps.c psx_save_context_to_tcb);
 *     a sentinel there resumes the thread on a non-code address, which is the
 *     "execution completed, PC=0" class in docs/BOOT_BLOCKERS.md section 3;
 *   - an interrupt delivered at the call's return boundary uses the same value
 *     as EPC (interrupts.c psx_check_interrupts_at), and the BIOS handler
 *     stores it in the TCB architecturally.
 *
 * psx_guest_call_enter()/leave() bracket the dispatch and publish the caller's
 * real continuation; psx_guest_call_resume_pc() returns it (outermost frame
 * first, so nested host calls cannot hide it). Both consumers substitute it
 * only when the value they were given is not a plausible guest PC, so guest
 * paths are byte-for-byte unchanged.
 *
 * Register contract. A host call presents the C body's guest registers: a0..a3
 * are the call's arguments, $ra is the call's return address (a sentinel), and
 * $sp is the C body's guest stack pointer. Because a decompiled body never runs
 * its own `addiu sp,sp,-N` prologue, that $sp is the CALLER's frame top, not a
 * frame of its own: a callee that pushes a frame, and an ISR taken inside the
 * call (which saves/restores $sp in the thread's TCB), would land in the live
 * caller frame. psx_mod_call_guest therefore reserves a real frame below $sp
 * for the duration of the call and moves the outgoing argument area (16($sp),
 * eight slots) with it, so the callee sees the layout a guest `jal` gives it.
 * $gp and $s8 are passed through unchanged; nothing else is synthesised.
 *
 * Non-returning calls. A vector that blocks or switches threads (ChangeThread
 * A0:03, ReturnFromException C0:03, WaitEvent B0:06, and anything that ends in
 * a thread switch) does not come back to the C caller: the scheduler longjmps
 * out and the C frame is abandoned. psx_mod_call_guest_ex() reports that
 * through *returned = 0 and psx_guest_call_stats() counts it; a caller that
 * must not continue after such a call has to use the _ex form and stop when
 * *returned is 0 (psx_mod_call_guest keeps the historical behaviour). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxGuestCallStats {
    uint64_t calls;          /* host-initiated guest calls */
    uint64_t returns;        /* the guest came back to the call's return address */
    uint64_t non_returning;  /* thread switch / wild exit: the C caller cannot continue */
    uint64_t bad_ra;         /* no real guest continuation was available to substitute */
    uint64_t bad_sp;         /* entered with a guest $sp that is not a plausible stack */
    uint64_t irq_in_call;    /* interrupts delivered while a host call was in flight */
    uint64_t isr_returns;    /* RFE completions whose target was a plausible guest PC */
    uint64_t isr_bad_target; /* RFE / vector target that was not a plausible guest PC */
} PsxGuestCallStats;

void psx_guest_call_stats(PsxGuestCallStats* out);

/* Aligned and inside live main RAM or the BIOS ROM window. The same rule the
 * interrupt EPC selection uses; a hybrid call sentinel fails it. */
int psx_guest_pc_plausible(uint32_t pc);

/* Nonzero while at least one host-initiated call is in flight. The global is
 * the hot-path form (psx_check_interrupts_at runs at every block leader). */
extern int g_psx_guest_call_depth;
int psx_guest_call_depth(void);

/* The guest PC the interrupted/suspended thread must resume at, or 0 when no
 * host call is in flight (or none had a real continuation). */
uint32_t psx_guest_call_resume_pc(void);

/* Bracket a host-initiated call. enter() returns 1 when it pushed a frame;
 * pass that value back to leave(). caller_ra is the calling C frame's $ra and
 * sp its guest $sp (counted as bad_sp when it is not a plausible stack). */
int  psx_guest_call_enter(uint32_t caller_ra, uint32_t sp);
void psx_guest_call_leave(int pushed);

/* An interrupt was delivered while a host call was in flight (interrupts.c). */
void psx_guest_call_note_irq(void);

/* Bookkeeping for the ISR return path: `ok` = the RFE target (the kernel's
 * ReturnFromException loads it from the thread TCB into k0 and jumps) was a
 * plausible guest PC. A bad target is substituted before the jump
 * (interrupts.c psx_rfe_escape_check) and counted here. */
void psx_guest_call_note_isr_return(int ok);

/* Why the guest last ended at PC 0 (docs/BOOT_BLOCKERS.md section 5). */
enum {
    PSX_PC0_SRC_UNKNOWN = 0,
    PSX_PC0_SRC_DISPATCH = 1,   /* psx_unknown_dispatch saw addr 0 */
    PSX_PC0_SRC_RA = 2,         /* ... and $ra was 0 too: a null return address */
    PSX_PC0_SRC_K0 = 3,         /* RFE target (TCB resume slot -> k0) was not a guest PC */
    PSX_PC0_SRC_EPC = 4,        /* exception vector decode produced no guest PC */
    PSX_PC0_SRC_TCB = 5         /* scheduler restored a TCB with no guest resume PC */
};
extern uint32_t g_pc0_src;

/* Bookkeeping from the call wrapper: did the guest return to the call's
 * return address? */
void psx_guest_call_note_return(int returned);

/* A thread context was saved while a host call was in flight: the C caller
 * will not continue (counted as non-returning). */
void psx_guest_call_note_suspended(void);

#ifdef __cplusplus
}
#endif

#endif
