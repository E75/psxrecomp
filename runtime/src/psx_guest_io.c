/* psx_guest_io.c — see psx_guest_io.h.
 *
 * Why a vectored exception handler instead of a mapping: the device window has
 * to be serviced, not mirrored. Mapping 16 KiB of host memory at
 * 0x1F801000 would stop the fault and silently swallow every store to a device
 * (I_MASK, GP0, DMA, CD, SPU, timers), which is exactly the class of bug this
 * closes. Instead the fault is decoded and replayed through the CPU's own
 * accessors, so a decompiled-C dereference of a device register has the same
 * effect as the guest instruction it came from.
 *
 * Scope is deliberately tiny: only MOV / MOVZX / MOVSX forms (the shapes a
 * pointer dereference compiles to) on a single memory operand whose effective
 * address is the faulting address. Anything else is passed on unchanged, so a
 * genuine wild pointer still reaches the crash tracer.
 */

#include "psx_guest_io.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_memory.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* memory.c: the backing stores the guest windows are mapped to. */
extern uint8_t* memory_get_ram_ptr(void);
extern uint8_t* memory_get_scratchpad_ptr(void);
extern uint8_t* memory_get_bios_ptr(void);

/* memory.c: the guest access path (device side effects included). */
extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t value);
extern void     psx_write_half(uint32_t addr, uint16_t value);
extern void     psx_write_byte(uint32_t addr, uint8_t value);

#define PSX_BIOS_ROM_BYTES 0x80000u
#define PSX_SCRATCH_BYTES  0x400u
#define PSX_CACHE_CTRL     0xFFFE0130u

int psx_guest_addr_is_io(uint32_t addr)
{
    return addr >= PSX_IO_WINDOW_BASE && addr <= PSX_IO_WINDOW_END;
}

void* psx_guest_ptr(uint32_t addr, uint32_t bytes)
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!bytes || bytes > 4u) return 0;
    if (phys < psx_ram_live_bytes() && bytes <= psx_ram_live_bytes() - phys)
        return memory_get_ram_ptr() + psx_ram_map_read(phys);
    if (phys >= 0x1F800000u && phys - 0x1F800000u < PSX_SCRATCH_BYTES &&
        bytes <= PSX_SCRATCH_BYTES - (phys - 0x1F800000u))
        return memory_get_scratchpad_ptr() + (phys - 0x1F800000u);
    if (phys >= 0x1FC00000u && phys - 0x1FC00000u < PSX_BIOS_ROM_BYTES &&
        bytes <= PSX_BIOS_ROM_BYTES - (phys - 0x1FC00000u))
        return memory_get_bios_ptr() + (phys - 0x1FC00000u);
    return 0;   /* I/O window, KSEG2 and unmapped space: use the accessors */
}

void psx_guest_write_word(uint32_t addr, uint32_t value) { psx_write_word(addr, value); }
void psx_guest_write_half(uint32_t addr, uint16_t value) { psx_write_half(addr, value); }
void psx_guest_write_byte(uint32_t addr, uint8_t value)  { psx_write_byte(addr, value); }

static uint64_t s_reads, s_writes, s_refused;

void psx_guest_io_stats(uint64_t* reads, uint64_t* writes, uint64_t* refused)
{
    if (reads) *reads = s_reads;
    if (writes) *writes = s_writes;
    if (refused) *refused = s_refused;
}

#ifdef _WIN32

typedef struct GuestIoOp {
    uint8_t  len;        /* whole instruction, prefixes included */
    uint8_t  width;      /* 1, 2 or 4 bytes */
    uint8_t  is_write;
    uint8_t  has_dest;   /* load: result goes to a GPR */
    uint8_t  sign_extend;
    uint8_t  preserve_upper; /* 8/16-bit MOV into a register keeps the upper bits */
    uint8_t  dest;       /* GPR index 0..15 (rax..rdi, r8..r15) */
    uint32_t addr;
    uint32_t value;
} GuestIoOp;

static uint32_t io_u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* GPR by encoding index: CONTEXT keeps Rax,Rcx,Rdx,Rbx,Rsp,Rbp,Rsi,Rdi,R8..R15
 * in exactly the architectural order, so the array is directly indexable. */
static uint64_t* io_gpr(CONTEXT* ctx, int idx) { return &(&ctx->Rax)[idx]; }

/* Decode one instruction at `code`. Returns 1 and fills `op` when it is a
 * MOV/MOVZX/MOVSX with one memory operand; the caller compares op->addr with
 * the faulting address before doing anything. */
static int io_decode(const uint8_t* code, CONTEXT* ctx, GuestIoOp* op)
{
    size_t i = 0;
    int size16 = 0;
    uint8_t rex = 0;
    uint8_t b;

    for (;;) {
        b = code[i];
        if (b == 0x66) { size16 = 1; i++; continue; }
        if (b >= 0x40 && b <= 0x4F) { rex = b; i++; continue; }
        break;
    }
    if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E ||
        b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65)
        return 0;                      /* address-size/segment/lock prefixes */
    i++;                               /* opcode byte */

    memset(op, 0, sizeof *op);
    op->addr = 0;

    int movsx = 0;
    if (b == 0x0F) {
        uint8_t b2 = code[i++];
        if (b2 == 0xB6) { op->width = 1; }
        else if (b2 == 0xB7) { op->width = 2; }
        else if (b2 == 0xBE) { movsx = 1; op->width = 1; }
        else if (b2 == 0xBF) { movsx = 1; op->width = 2; }
        else return 0;
        op->has_dest = 1;
        op->sign_extend = (uint8_t)movsx;
        b = 0x8B;                      /* r/m -> reg, register-sized result */
    } else if (b == 0x88) {
        op->width = 1; op->is_write = 1;
    } else if (b == 0x89) {
        if (rex & 0x08) return 0;      /* 64-bit store: not a device access */
        op->width = size16 ? 2u : 4u; op->is_write = 1;
    } else if (b == 0x8A) {
        op->width = 1; op->has_dest = 1; op->preserve_upper = 1;
    } else if (b == 0x8B) {
        if (rex & 0x08) return 0;      /* 64-bit load */
        op->width = size16 ? 2u : 4u; op->has_dest = 1;
        op->preserve_upper = size16 ? 1u : 0u;
    } else if (b == 0xC6 || b == 0xC7) {
        op->width = (b == 0xC6) ? 1u : (size16 ? 2u : 4u);
        op->is_write = 1;
    } else {
        return 0;
    }

    /* ModRM (+ SIB + displacement) -> effective address. */
    uint8_t m = code[i++];
    int mod = m >> 6;
    int reg = ((m >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
    int rm = m & 7;
    int rip_relative = 0, have_base = 1;
    int base = rm | ((rex & 0x01) ? 8 : 0);
    uint64_t ea = 0;

    if (mod == 3) return 0;            /* register operand: not a faulting load */

    if (rm == 4) {
        uint8_t sib = code[i++];
        int scale = sib >> 6;
        int index = ((sib >> 3) & 7) | ((rex & 0x02) ? 8 : 0);
        int sbase = sib & 7;
        if (index != 4) ea += *io_gpr(ctx, index) << scale;
        if (sbase == 5 && mod == 0) have_base = 0;
        else base = sbase | ((rex & 0x01) ? 8 : 0);
    } else if (rm == 5 && mod == 0) {
        rip_relative = 1;
        have_base = 0;
    }

    int32_t disp = 0;
    if (mod == 1) {
        if (i + 1u > 15u) return 0;
        disp = (int8_t)code[i];
        i += 1;
    } else if (mod == 2 || (mod == 0 && !have_base)) {
        if (i + 4u > 15u) return 0;
        disp = (int32_t)io_u32(code + i);
        i += 4;
    }

    if (rip_relative) ea = (uint64_t)(ctx->Rip + i + (uint64_t)(int64_t)disp);
    else if (have_base) ea += *io_gpr(ctx, base);
    if (!rip_relative) ea = (uint64_t)((int64_t)ea + disp);

    op->addr = (uint32_t)ea;
    if ((uint64_t)op->addr != ea) return 0;   /* not a 32-bit guest address */

    if (b == 0xC6 || b == 0xC7) {
        uint8_t imm[4] = { 0, 0, 0, 0 };
        if (i + op->width > 15u) return 0;
        memcpy(imm, code + i, op->width);
        i += op->width;
        memcpy(&op->value, imm, sizeof op->value);
        if (op->width == 1) op->value &= 0xFFu;
        else if (op->width == 2) op->value &= 0xFFFFu;
    } else if (op->is_write) {
        op->value = (uint32_t)*io_gpr(ctx, reg);
        if (op->width == 1) op->value &= 0xFFu;
        else if (op->width == 2) op->value &= 0xFFFFu;
    } else {
        op->dest = (uint8_t)reg;
    }

    if (i > 15u) return 0;
    op->len = (uint8_t)i;
    return 1;
}

static int io_service(GuestIoOp* op)
{
    if (op->is_write) {
        if (op->width == 1) psx_write_byte(op->addr, (uint8_t)op->value);
        else if (op->width == 2) psx_write_half(op->addr, (uint16_t)op->value);
        else psx_write_word(op->addr, op->value);
        s_writes++;
        return 1;
    }
    uint32_t v;
    if (op->width == 1) v = psx_read_byte(op->addr);
    else if (op->width == 2) v = psx_read_half(op->addr);
    else v = psx_read_word(op->addr);
    if (op->sign_extend) {
        if (op->width == 1) v = (uint32_t)(int32_t)(int8_t)v;
        else v = (uint32_t)(int32_t)(int16_t)v;
    }
    s_reads++;
    return (int)v;
}

/* Re-entrancy: the guard covers decoding too, because the instruction bytes at
 * RIP may straddle an unmapped page and fault inside the handler. A second
 * fault is a real bug (or a decoder mistake) and must reach the crash tracer. */
static int s_active;

static LONG io_handle(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;

    const ULONG_PTR fault = info->ExceptionRecord->ExceptionInformation[1];
    if (fault > 0xFFFFFFFFull) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t addr = (uint32_t)fault;
    if (!psx_guest_addr_is_io(addr) && addr != PSX_CACHE_CTRL) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* ctx = info->ContextRecord;
    GuestIoOp op;
    if (!io_decode((const uint8_t*)(uintptr_t)ctx->Rip, ctx, &op)) {
        s_refused++;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (op.addr != addr) {                 /* decoded something else: do not guess */
        s_refused++;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const int loaded = io_service(&op);
    if (!op.is_write) {
        /* x86: a 32-bit write zeroes the upper half, an 8/16-bit write keeps it. */
        uint64_t* reg = io_gpr(ctx, op.dest);
        if (op.preserve_upper) {
            const uint64_t mask = op.width == 1 ? 0xFFull : 0xFFFFull;
            *reg = (*reg & ~mask) | ((uint32_t)loaded & mask);
        } else {
            *reg = (uint64_t)(uint32_t)loaded;
        }
    }
    ctx->Rip += op.len;
    if (s_writes + s_reads == 1u) {
        fprintf(stderr, "psx_guest_io: serviced host %s of 0x%08X (width %u)\n",
                op.is_write ? "store" : "load", op.addr, (unsigned)op.width);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK guest_io_vectored(EXCEPTION_POINTERS* info)
{
    if (s_active) return EXCEPTION_CONTINUE_SEARCH;
    s_active = 1;
    const LONG r = io_handle(info);
    s_active = 0;
    return r;
}

static int s_installed;

int psx_guest_io_install(void)
{
    if (s_installed) return 1;
    const char* off = getenv("PSX_IO_FAULT_OFF");
    if (off && *off && *off != '0') return 0;
    if (!AddVectoredExceptionHandler(1, guest_io_vectored)) return 0;
    s_installed = 1;
    return 1;
}

#else  /* !_WIN32 */

int psx_guest_io_install(void) { return 0; }

#endif
