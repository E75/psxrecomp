/* psx_static_guest.h - measure (and optionally forbid) host-to-guest entries.
 *
 * The static-port goal is "every Konami instruction runs as compiled C; the
 * emulator only serves Sony/PsyQ (BIOS kernel, GPU, SPU, CD, MDEC, DMA,
 * timers, IRQ, SIO, bus)". The funnels in this header are the only ways a
 * HOST frame can hand control to guest code, so they are the thing to count
 * and, eventually, the thing to gate.
 *
 * Instrumentation only: with PSX_STATIC_GUEST unset the mode is 0 and every
 * call site tests one int before calling in. Nothing here changes execution.
 *
 *   PSX_STATIC_GUEST=log     count, keep a ring, print the first unique
 *                            non-BIOS target and every 1000th hit
 *   PSX_STATIC_GUEST=abort   print funnel/caller/target and abort() on the
 *                            first non-BIOS-class target
 *   PSX_STATIC_GUEST_DUMP=<path>
 *                            rewrite a CSV of (funnel,target,class,count,
 *                            last caller) whenever a new target appears and at
 *                            exit. Rewriting (not appending) keeps it readable
 *                            while the game runs and after a hard kill.
 *
 * This header is a measurement surface (docs/STATIC_PORT.md), not an HLE
 * implementation: it never supplies a body and never consumes a call.
 */
#ifndef PSXRECOMP_PSX_STATIC_GUEST_H
#define PSXRECOMP_PSX_STATIC_GUEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSX_SG_FUNNEL_DISPATCH_CALL = 0, /* psx_dispatch_call: every dynamic host dispatch */
    PSX_SG_FUNNEL_MOD_CALL_GUEST = 1, /* psx_mod_call_guest: WE_GCALL / we_icall / we_bios_tail */
    PSX_SG_FUNNEL_INTERP = 2,         /* the guest actually ran instructions */
    PSX_SG_FUNNEL_SPAN = 3,           /* dirty_ram_run_span: a render-pass replay */
    PSX_SG_FUNNEL_NATIVE_HANDOFF = 4, /* dirty_ram_dispatch answered with native code:
                                       * a router, not guest execution (0 instructions) */
    PSX_SG_FUNNEL_COUNT = 5
};

enum {
    PSX_SG_CLASS_BIOS = 0,  /* Sony/PsyQ: kernel window, ROM, A0/B0/C0 vectors */
    PSX_SG_CLASS_GAME = 1,  /* anything else: Konami code, must become C */
    PSX_SG_CLASS_UNKNOWN = 2
};

/* 0 = inert, 1 = log, 2 = abort. Read from the environment on first use. */
extern int g_psx_static_guest_mode;

int  psx_static_guest_mode(void);
const char *psx_static_guest_funnel_name(int funnel);

/* Classify a target by owner. Deliberately conservative: only the relocated
 * kernel window, BIOS ROM and the A0/B0/C0 vectors count as Sony/PsyQ. */
int psx_static_guest_class(uint32_t target);

/* Record one entry. `caller` is the guest $ra (or dispatch return address). */
void psx_static_guest_note(int funnel, uint32_t target, uint32_t caller);

uint64_t psx_static_guest_total(int funnel);
uint64_t psx_static_guest_nonbios_total(void);
uint32_t psx_static_guest_distinct(void);

/* Force a dump (used by atexit, and available to a caller that exits). */
void psx_static_guest_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_STATIC_GUEST_H */
