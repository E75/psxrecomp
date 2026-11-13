#ifndef PSXRECOMP_PSX_GUEST_IO_H
#define PSXRECOMP_PSX_GUEST_IO_H

/* The guest I/O window as seen by host C.
 *
 * memory.c maps the guest's RAM, scratchpad and BIOS ROM at their real
 * addresses so decompiled C can keep pointer-typed locals and dereference them
 * directly (see the note above psx_map_fixed). The I/O window
 * 0x1F801000..0x1F803FFF is the one guest window that cannot be a plain host
 * mirror: a store there is a device command, not a byte in memory.
 *
 * Decompiled C that reaches the window through a computed pointer therefore
 * dereferences an address that has no host mapping, and the process takes an
 * access violation. The recompiled/guest path never does this: every store and
 * load is emitted as cpu->write_word / cpu->read_word, which services the
 * device. This module closes that gap at the runtime boundary:
 *
 *   1. psx_guest_io_install() (called from memory_init) installs a first-chance
 *      vectored exception handler that recognises an access violation on the
 *      I/O window, decodes the faulting MOV, performs the access through the
 *      SAME psx_read/psx_write accessors the CPU uses, and resumes after it. A host
 *      pointer dereference of a device register is then indistinguishable from
 *      a guest access, side effects included. Anything it cannot decode is
 *      passed on unchanged, so this never masks a real fault.
 *   2. psx_guest_ptr() is the opt-in form for new code: a host pointer for
 *      RAM/scratchpad/BIOS, NULL for the window. Callers use the psx_guest_*
 *      accessors in that case.
 *
 * PSX_IO_FAULT_OFF=1 disables (1) for an A/B run. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_IO_WINDOW_BASE 0x1F801000u
#define PSX_IO_WINDOW_END  0x1F803FFFu

/* 1 when addr (any segment) is a device register in the I/O window. */
int psx_guest_addr_is_io(uint32_t addr);

/* Host pointer for a guest address, or NULL when it has no host mapping
 * (the I/O window, KSEG2, unmapped space). The returned pointer is valid for
 * the live RAM geometry: pass `bytes` (1..4) so a span that crosses the live
 * RAM end or the scratchpad end is refused instead of aliasing. */
void* psx_guest_ptr(uint32_t addr, uint32_t bytes);

/* Device accessors: identical to the CPU's own load/store path (side effects,
 * provenance, IsC isolation and timing included). Use these where the pointer
 * form cannot be used. */
uint32_t psx_guest_read_word(uint32_t addr);
uint16_t psx_guest_read_half(uint32_t addr);
uint8_t  psx_guest_read_byte(uint32_t addr);
void     psx_guest_write_word(uint32_t addr, uint32_t value);
void     psx_guest_write_half(uint32_t addr, uint16_t value);
void     psx_guest_write_byte(uint32_t addr, uint8_t value);

/* Idempotent; safe to call from any thread. Returns 1 when the handler is
 * installed (or was already), 0 on a platform without vectored handlers or
 * when disabled by PSX_IO_FAULT_OFF. */
int psx_guest_io_install(void);

/* Observability: how many host dereferences of the window were serviced, and
 * how many were refused (a refusal is passed on to the crash tracer). */
void psx_guest_io_stats(uint64_t* reads, uint64_t* writes, uint64_t* refused);

/* The last refused instruction: its rip as an image RVA, the faulting address,
 * and the 16 bytes at rip (zeroed when nothing was refused yet). The same
 * values are printed to stderr for the first 8 refusals. */
void psx_guest_io_last_refusal(uint32_t* rip_rva, uint32_t* fault, uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif
