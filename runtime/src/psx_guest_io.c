/* psx_guest_io.c — see psx_guest_io.h.
 *
 * Why a vectored exception handler instead of a mapping: the device window has
 * to be serviced, not mirrored. Mapping 16 KiB of host memory at 0x1F801000
 * would stop the fault and silently swallow every store to a device (I_MASK,
 * GP0, DMA, CD, SPU, timers), which is exactly the class of bug this closes.
 * Instead the fault is decoded and replayed through the CPU's own accessors, so
 * a decompiled-C dereference of a device register has the same effect as the
 * guest instruction it came from.
 *
 * Coverage is the access family a compiler emits for a pointer dereference on
 * this target (MSVC/Clang x86-64, 32-bit guest data):
 *   MOV / MOVZX / MOVSX                8/16/32/64-bit, SIB, disp8/disp32,
 *                                      imm8/imm16/imm32 (incl. REX.W imm32)
 *   AND/OR/XOR/ADD/SUB  [m], r|imm     read-modify-write: ONE device read +
 *                                      ONE device write, flags reproduced
 *   TEST/CMP            [m], r|imm     read only, flags reproduced
 *   INC/DEC             [m]            read-modify-write, CF preserved
 *   XCHG                [m], r         read + write, result to the register
 * plus the 0x66 (operand size) and 0x67 (32-bit addressing) prefixes.
 *
 * A 64-bit access is replayed as the two adjacent 32-bit device accesses the
 * compiler merged (low dword first) — that is what the guest's two stores did.
 *
 * Everything else is refused: flag-setting ADC/SBB (carry in), string ops,
 * SSE/AVX forms, a decoded effective address that is not the faulting address,
 * a fault outside the window. A refusal is counted, its bytes are dumped once
 * (hex + rip RVA) and the fault is passed on, so a real wild pointer still
 * reaches the crash tracer and lane A can see what shape is left.
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

/* One device access. Width 8 is the compiler's merge of two adjacent 32-bit
 * stores: replay it as those two accesses, low dword first. */
static void io_dev_write(uint32_t addr, uint8_t width, uint64_t value)
{
    if (width == 1u) psx_write_byte(addr, (uint8_t)value);
    else if (width == 2u) psx_write_half(addr, (uint16_t)value);
    else if (width == 4u) psx_write_word(addr, (uint32_t)value);
    else {
        psx_write_word(addr, (uint32_t)value);
        psx_write_word(addr + 4u, (uint32_t)(value >> 32));
    }
}

static uint64_t io_dev_read(uint32_t addr, uint8_t width)
{
    if (width == 1u) return psx_read_byte(addr);
    if (width == 2u) return psx_read_half(addr);
    if (width == 4u) return psx_read_word(addr);
    return (uint64_t)psx_read_word(addr) |
           ((uint64_t)psx_read_word(addr + 4u) << 32);
}

static uint64_t s_reads, s_writes, s_refused;

void psx_guest_io_stats(uint64_t* reads, uint64_t* writes, uint64_t* refused)
{
    if (reads) *reads = s_reads;
    if (writes) *writes = s_writes;
    if (refused) *refused = s_refused;
}

#ifdef _WIN32

/* Instruction kinds. MOV/TEST/CMP/XCHG/RMW differ only in the service step. */
enum {
    IO_MOV = 0,
    IO_ADD, IO_SUB, IO_AND, IO_OR, IO_XOR,
    IO_CMP, IO_TEST, IO_XCHG, IO_INC, IO_DEC
};

typedef struct GuestIoOp {
    uint8_t  len;            /* whole instruction, prefixes included */
    uint8_t  width;          /* 1, 2, 4 or 8 bytes */
    uint8_t  kind;
    uint8_t  is_write;       /* the instruction writes memory */
    uint8_t  has_dest;       /* result goes to a GPR */
    uint8_t  sign_extend;
    uint8_t  preserve_upper; /* 8/16-bit MOV into a register keeps upper bits */
    uint8_t  wide_dest;      /* REX.W destination register */
    uint8_t  dest;           /* GPR index 0..15 (rax..rdi, r8..r15) */
    uint8_t  imm;            /* operand is an immediate */
    uint32_t addr;
    uint64_t value;          /* store value / immediate */
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

static uint64_t io_mask(uint8_t width)
{
    return width >= 8u ? ~0ull : ((1ull << (width * 8u)) - 1ull);
}

/* ModRM (+ SIB + displacement). Returns 1 for a memory operand. */
static int io_modrm(const uint8_t* code, size_t* pi, CONTEXT* ctx, uint8_t rex,
                    int addr32, int* reg_out, uint32_t* ea_out)
{
    size_t i = *pi;
    const uint8_t m = code[i++];
    const int mod = m >> 6;
    const int reg = ((m >> 3) & 7) | ((rex & 0x04) ? 8 : 0);
    const int rm = m & 7;
    int have_base = 1, rip_relative = 0;
    int base = rm | ((rex & 0x01) ? 8 : 0);
    uint64_t ea = 0;

    if (reg_out) *reg_out = reg;
    if (mod == 3) return 0;                     /* register operand */

    if (rm == 4) {                              /* SIB */
        if (i + 1u > 15u) return 0;
        const uint8_t sib = code[i++];
        const int scale = sib >> 6;
        const int sraw = (sib >> 3) & 7;
        const int index = sraw | ((rex & 0x02) ? 8 : 0);
        const int sbase = sib & 7;
        if (sraw != 4) {
            const uint64_t idx = *io_gpr(ctx, index);
            ea += (addr32 ? (uint32_t)idx : idx) << scale;
        }
        if (sbase == 5 && mod == 0) have_base = 0;
        else base = sbase | ((rex & 0x01) ? 8 : 0);
    } else if (rm == 5 && mod == 0) {
        if (addr32) return 0;                   /* EIP-relative: never emitted here */
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
    else {
        if (have_base) {
            const uint64_t b = *io_gpr(ctx, base);
            ea += addr32 ? (uint32_t)b : b;
        }
        ea = (uint64_t)((int64_t)ea + disp);
    }

    if (ea > 0xFFFFFFFFull) return 0;           /* not a 32-bit guest address */
    *ea_out = (uint32_t)ea;
    *pi = i;
    return 1;
}

/* Decode one instruction at `code`. Returns 1 when it is a single-memory-operand
 * access from the supported family; the caller compares op->addr with the
 * faulting address before doing anything. */
static int io_decode(const uint8_t* code, CONTEXT* ctx, GuestIoOp* op)
{
    size_t i = 0;
    int size16 = 0, addr32 = 0;
    uint8_t rex = 0;

    for (;;) {
        if (i > 14u) return 0;                  /* prefix run cannot exceed 15 bytes */
        const uint8_t b = code[i];
        if (b == 0x66) { size16 = 1; i++; continue; }
        if (b == 0x67) { addr32 = 1; i++; continue; }
        if (b >= 0x40 && b <= 0x4F) { rex = b; i++; continue; }
        break;
    }
    uint8_t b = code[i];
    if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
        b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65)
        return 0;
    i++;

    memset(op, 0, sizeof *op);
    const uint8_t opsize = (rex & 0x08) ? 8u : (size16 ? 2u : 4u);
    op->wide_dest = (uint8_t)((rex & 0x08) ? 1u : 0u);
    int reg = 0;
    uint32_t ea = 0;
    uint32_t imm = 0;
    int have_imm = 0;
    uint8_t imm_bytes = 0;

    switch (b) {
    case 0x88: op->kind = IO_MOV; op->width = 1; break;
    case 0x89: op->kind = IO_MOV; op->width = opsize; break;
    case 0x8A: op->kind = IO_MOV; op->width = 1; op->has_dest = 1; op->preserve_upper = 1; break;
    case 0x8B: op->kind = IO_MOV; op->width = opsize; op->has_dest = 1;
               op->preserve_upper = (uint8_t)(opsize < 4u ? 1u : 0u); break;
    case 0x00: op->kind = IO_ADD; op->width = 1; break;
    case 0x01: op->kind = IO_ADD; op->width = opsize; break;
    case 0x08: op->kind = IO_OR;  op->width = 1; break;
    case 0x09: op->kind = IO_OR;  op->width = opsize; break;
    case 0x20: op->kind = IO_AND; op->width = 1; break;
    case 0x21: op->kind = IO_AND; op->width = opsize; break;
    case 0x28: op->kind = IO_SUB; op->width = 1; break;
    case 0x29: op->kind = IO_SUB; op->width = opsize; break;
    case 0x30: op->kind = IO_XOR; op->width = 1; break;
    case 0x31: op->kind = IO_XOR; op->width = opsize; break;
    case 0x38: op->kind = IO_CMP; op->width = 1; break;
    case 0x39: op->kind = IO_CMP; op->width = opsize; break;
    case 0x84: op->kind = IO_TEST; op->width = 1; break;
    case 0x85: op->kind = IO_TEST; op->width = opsize; break;
    case 0x86: op->kind = IO_XCHG; op->width = 1; break;
    case 0x87: op->kind = IO_XCHG; op->width = opsize; break;
    case 0xC6: op->kind = IO_MOV; op->width = 1; have_imm = 1; imm_bytes = 1; break;
    case 0xC7: op->kind = IO_MOV; op->width = opsize; have_imm = 1;
               imm_bytes = (uint8_t)(size16 ? 2u : 4u); break;
    case 0x80: op->width = 1; have_imm = 1; imm_bytes = 1; break;   /* group 1, imm8 */
    case 0x81: op->width = opsize; have_imm = 1;
               imm_bytes = (uint8_t)(size16 ? 2u : 4u); break;       /* group 1, imm16/32 */
    case 0x83: op->width = opsize; have_imm = 1; imm_bytes = 1; break; /* group 1, imm8 sx */
    case 0xF6: op->width = 1; have_imm = 1; imm_bytes = 1; break;   /* group 3, imm8 */
    case 0xF7: op->width = opsize; have_imm = 1;
               imm_bytes = (uint8_t)(size16 ? 2u : 4u); break;       /* group 3, imm16/32 */
    case 0xFE: op->width = 1; break;                       /* inc/dec r/m8 */
    case 0xFF: op->width = opsize; break;                  /* inc/dec r/m */
    case 0x0F: {
        if (i + 1u > 15u) return 0;
        const uint8_t b2 = code[i++];
        op->kind = IO_MOV;
        op->has_dest = 1;
        if (b2 == 0xB6) op->width = 1;
        else if (b2 == 0xB7) op->width = 2;
        else if (b2 == 0xBE) { op->width = 1; op->sign_extend = 1; }
        else if (b2 == 0xBF) { op->width = 2; op->sign_extend = 1; }
        else return 0;
        break;
    }
    default:
        return 0;
    }

    if (!io_modrm(code, &i, ctx, rex, addr32, &reg, &ea)) return 0;
    op->addr = ea;

    /* Group opcodes are selected by the ModRM reg field. */
    if (b == 0x80 || b == 0x81 || b == 0x83) {
        static const uint8_t kind[8] = { IO_ADD, IO_OR, 0xFF, 0xFF, IO_AND, IO_SUB, IO_XOR, IO_CMP };
        if (kind[reg & 7] == 0xFF) return 0;    /* ADC/SBB need the carry in */
        op->kind = kind[reg & 7];
    } else if (b == 0xF6 || b == 0xF7) {
        if ((reg & 7) != 0u) return 0;          /* NOT/NEG/MUL/IMUL/DIV/IDIV */
        op->kind = IO_TEST;
    } else if (b == 0xFE || b == 0xFF) {
        if ((reg & 7) > 1u) return 0;           /* not inc/dec */
        op->kind = ((reg & 7) == 0u) ? IO_INC : IO_DEC;
    }
    if (op->kind == IO_XCHG) op->has_dest = 1;  /* the register gets the old value */

    if (have_imm) {
        if (i + imm_bytes > 15u) return 0;
        if (imm_bytes == 1u) imm = code[i];
        else if (imm_bytes == 2u) { uint16_t v; memcpy(&v, code + i, sizeof v); imm = v; }
        else imm = io_u32(code + i);
        i += imm_bytes;
        op->imm = 1;
        /* REX.W with an imm32 group-1/C7 operand sign-extends to 64 bits; the
         * 0x83 form sign-extends its imm8 to the operand size. */
        op->value = (b == 0x83) ? (uint64_t)(int64_t)(int8_t)imm
                                : (uint64_t)(uint32_t)imm;
        if (op->width == 8u && b != 0x83)
            op->value = (uint64_t)(int64_t)(int32_t)imm;
    } else if (op->kind == IO_INC || op->kind == IO_DEC) {
        /* group 5: the ModRM reg field is the opcode extension, not a register */
        op->value = 1;
    } else if (op->kind != IO_CMP && op->kind != IO_TEST) {
        op->value = *io_gpr(ctx, reg) & io_mask(op->width);
    }

    op->is_write = (uint8_t)(op->kind == IO_ADD || op->kind == IO_SUB ||
                             op->kind == IO_AND || op->kind == IO_OR ||
                             op->kind == IO_XOR || op->kind == IO_INC ||
                             op->kind == IO_DEC || op->kind == IO_XCHG ||
                             (op->kind == IO_MOV && !op->has_dest));
    if (op->has_dest) op->dest = (uint8_t)reg;

    if (i > 15u) return 0;
    op->len = (uint8_t)i;
    return 1;
}

/* Arithmetic/logic flags for the RMW forms. The decompiled C may branch on the
 * result, so the host flags have to be the ones the skipped instruction set. */
static void io_set_flags(CONTEXT* ctx, uint8_t kind, uint64_t a, uint64_t b,
                         uint64_t r, uint8_t width)
{
    const uint8_t bits = (uint8_t)(width * 8u);
    const uint64_t mask = io_mask(width);
    a &= mask;
    b &= mask;
    r &= mask;
    const uint64_t sign = 1ull << (bits - 1u);

    int cf, of, af, sf, zf, pf;
    switch (kind) {
    case IO_ADD:
        cf = (r < a);
        of = (((a ^ r) & (b ^ r)) & sign) != 0u;
        af = (((a ^ b ^ r) >> 4) & 1u) != 0u;
        break;
    case IO_SUB:
    case IO_CMP:
        cf = (a < b);
        of = (((a ^ b) & (a ^ r)) & sign) != 0u;
        af = (((a ^ b ^ r) >> 4) & 1u) != 0u;
        break;
    case IO_INC:
        cf = (int)((ctx->EFlags >> 0) & 1u);
        of = (r == sign);
        af = (((a ^ b ^ r) >> 4) & 1u) != 0u;
        break;
    case IO_DEC:
        cf = (int)((ctx->EFlags >> 0) & 1u);
        of = (r == (sign - 1u));
        af = (((a ^ b ^ r) >> 4) & 1u) != 0u;
        break;
    default:   /* AND / OR / XOR / TEST */
        cf = 0;
        of = 0;
        af = 0;
        break;
    }
    sf = (r & sign) != 0u;
    zf = (r == 0u);
    pf = (__builtin_parityll(r & 0xFFu) != 0);

    uint32_t f = (uint32_t)ctx->EFlags;
    f &= ~(0x0001u | 0x0004u | 0x0010u | 0x0040u | 0x0080u | 0x0800u);
    f |= (uint32_t)(cf ? 0x0001u : 0u) | (uint32_t)(pf ? 0x0004u : 0u) |
         (uint32_t)(af ? 0x0010u : 0u) | (uint32_t)(zf ? 0x0040u : 0u) |
         (uint32_t)(sf ? 0x0080u : 0u) | (uint32_t)(of ? 0x0800u : 0u);
    ctx->EFlags = f;
}

/* Run one decoded access. `old` receives the value read (0 for a pure store),
 * `result` the value the instruction produces (the loaded value for a load, the
 * stored value for a store, the new memory value for a read-modify-write).
 * Exactly one device read and one device write per 32-bit register. */
static void io_service(GuestIoOp* op, uint64_t* old_out, uint64_t* result_out)
{
    const uint64_t mask = io_mask(op->width);
    uint64_t old = 0, result = 0;

    switch (op->kind) {
    case IO_MOV:
        if (op->is_write) {
            io_dev_write(op->addr, op->width, op->value);
            s_writes++;
            result = op->value;
        } else {
            s_reads++;
            result = io_dev_read(op->addr, op->width);
        }
        break;

    case IO_TEST:
        s_reads++;
        old = io_dev_read(op->addr, op->width);
        result = old & op->value;
        break;

    case IO_CMP:
        s_reads++;
        old = io_dev_read(op->addr, op->width);
        result = old - (op->value & mask);
        break;

    case IO_XCHG:
        s_reads++;
        old = io_dev_read(op->addr, op->width);
        io_dev_write(op->addr, op->width, op->value & mask);
        s_writes++;
        result = old;
        break;

    default: {   /* read-modify-write */
        s_reads++;
        old = io_dev_read(op->addr, op->width);
        switch (op->kind) {
        case IO_ADD: result = old + (op->value & mask); break;
        case IO_SUB: result = old - (op->value & mask); break;
        case IO_AND: result = old & op->value; break;
        case IO_OR:  result = old | op->value; break;
        case IO_XOR: result = old ^ op->value; break;
        case IO_INC: result = old + 1u; break;
        default:     result = old - 1u; break;   /* IO_DEC */
        }
        result &= mask;
        io_dev_write(op->addr, op->width, result);
        s_writes++;
        break;
    }
    }

    *old_out = old;
    *result_out = result;
}

/* Re-entrancy: the guard covers decoding too, because the instruction bytes at
 * RIP may straddle an unmapped page and fault inside the handler. A second
 * fault is a real bug (or a decoder mistake) and must reach the crash tracer. */
static int s_active;
static int s_refusal_logs;
static int s_trace = -1;

typedef struct IoRefusal {
    uint32_t rip_rva;
    uint32_t fault;
    uint8_t  bytes[16];
} IoRefusal;

static IoRefusal s_last_refusal;

void psx_guest_io_last_refusal(uint32_t* rip_rva, uint32_t* fault, uint8_t out[16])
{
    if (rip_rva) *rip_rva = s_last_refusal.rip_rva;
    if (fault) *fault = s_last_refusal.fault;
    if (out) memcpy(out, s_last_refusal.bytes, sizeof s_last_refusal.bytes);
}

static void io_refuse(EXCEPTION_POINTERS* info, uint32_t addr)
{
    s_refused++;
    const uint8_t* code = (const uint8_t*)(uintptr_t)info->ContextRecord->Rip;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    const uintptr_t rip = (uintptr_t)code;
    const uint32_t rva = (uint32_t)(base && rip >= base ? rip - base : rip);

    s_last_refusal.rip_rva = rva;
    s_last_refusal.fault = addr;
    memcpy(s_last_refusal.bytes, code, sizeof s_last_refusal.bytes);

    if (s_refusal_logs < 8) {
        s_refusal_logs++;
        fprintf(stderr, "psx_guest_io: REFUSED %s of 0x%08X at rip rva 0x%X bytes %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                (info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read"), addr,
                (unsigned)rva,
                s_last_refusal.bytes[0], s_last_refusal.bytes[1], s_last_refusal.bytes[2],
                s_last_refusal.bytes[3], s_last_refusal.bytes[4], s_last_refusal.bytes[5],
                s_last_refusal.bytes[6], s_last_refusal.bytes[7], s_last_refusal.bytes[8],
                s_last_refusal.bytes[9], s_last_refusal.bytes[10], s_last_refusal.bytes[11]);
        fflush(stderr);
    }
}

/* Publish the result into the destination register with x86 write semantics. */
static void io_writeback(CONTEXT* ctx, const GuestIoOp* op, uint64_t result)
{
    uint64_t* reg = io_gpr(ctx, op->dest);
    if (op->preserve_upper) {
        const uint64_t m = io_mask(op->width);
        *reg = (*reg & ~m) | (result & m);
    } else if (op->sign_extend) {
        /* movsx/movsbl: sign-extend from the operand width, not the result */
        const int64_t v = (op->width == 1u) ? (int64_t)(int8_t)result
                                            : (int64_t)(int16_t)result;
        *reg = op->wide_dest ? (uint64_t)v : (uint64_t)(uint32_t)(int32_t)v;
    } else {
        *reg = result;
    }
}

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
        io_refuse(info, addr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (op.addr != addr) {                 /* decoded something else: do not guess */
        io_refuse(info, addr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uint64_t old = 0, result = 0;
    io_service(&op, &old, &result);
    if (op.kind != IO_MOV && op.kind != IO_XCHG)
        io_set_flags(ctx, op.kind, old, op.value, result, op.width);
    if (op.has_dest || op.kind == IO_XCHG) io_writeback(ctx, &op, result);
    ctx->Rip += op.len;
    if (s_trace < 0) {
        const char* t = getenv("PSX_IO_FAULT_TRACE");
        s_trace = (t && *t && *t != '0') ? 1 : 0;
    }
    if (s_trace) {
        const uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        const uintptr_t rip = (uintptr_t)(ctx->Rip - op.len);
        fprintf(stderr, "psx_guest_io: %s 0x%08X w%u kind %u len %u at rva 0x%X\n",
                op.is_write ? "store" : "load", op.addr, (unsigned)op.width,
                (unsigned)op.kind, (unsigned)op.len,
                (unsigned)(base && rip >= base ? rip - base : rip));
        fflush(stderr);
    }
    if (s_writes + s_reads == 1u) {
        fprintf(stderr, "psx_guest_io: serviced host %s of 0x%08X (width %u, %s)\n",
                op.is_write ? "store" : "load", op.addr, (unsigned)op.width,
                op.kind == IO_MOV ? "mov" : "rmw");
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

void psx_guest_io_last_refusal(uint32_t* rip_rva, uint32_t* fault, uint8_t out[16])
{
    if (rip_rva) *rip_rva = 0;
    if (fault) *fault = 0;
    if (out) memset(out, 0, 16);
}

#endif
