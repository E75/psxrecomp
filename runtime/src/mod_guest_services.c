/* Guest services for seamless-loading adapters (mod_plugins.h). Each is the
 * one shared implementation of an effect several titles' adapters need; see
 * the header for the contract. */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "interrupts.h"
#include "psx_cycle_freeze.h"
#include "psx_guest_call.h"
#include "psx_memory.h"
#include "psx_cycles.h"
#include "psx_static_guest.h"
#include "spu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── host-initiated guest calls: the caller's real continuation ─────────────
 *
 * See psx_guest_call.h. A host call's $ra is a sentinel; anything that persists
 * a "resume PC" (TCB save on a thread switch, exception EPC at the call
 * boundary) must use the C frame's $ra instead. Kept as a small stack so a
 * nested host call cannot hide the outer continuation. */
#define PSX_GUEST_CALL_MAX_DEPTH 8
/* Frame reserved below the caller's $sp for every host call: >= the largest
 * prologue the C bodies' callers use, so the callee's frame never reaches the
 * live caller frame. */
#define PSX_HOST_CALL_FRAME     0x100u
#define PSX_HOST_CALL_ARG_BYTES 32u   /* 16($sp) .. 16($sp)+31: 8 stack args */

static uint32_t s_call_resume[PSX_GUEST_CALL_MAX_DEPTH];  /* 0 = unknown frame */
int             g_psx_guest_call_depth;
static PsxGuestCallStats s_call_stats;
static int      s_call_logs;
static uint64_t s_boundaries;          /* dispatch reached a call's return address */
static uint32_t s_sentinel[PSX_GUEST_CALL_MAX_DEPTH];

int psx_guest_pc_plausible(uint32_t pc)
{
    if (pc == 0u || (pc & 3u) != 0u) return 0;
    const uint32_t phys = pc & 0x1FFFFFFFu;
    /* Live RAM, the BIOS ROM window, and the bus range a trusted plugin may use
     * for mod-defined guest functions (mod_plugins.h). 0 never matches. */
    return phys < psx_ram_live_bytes() ||
           (phys >= 0x1FC00000u && phys < 0x1FC80000u) ||
           (phys >= 0x0F000000u && phys < 0x10000000u);
}

int psx_guest_call_depth(void) { return g_psx_guest_call_depth; }

/* ── transient-argument contract (psx_guest_call.h, docs/INIT_EXIT3.md 12) ──
 * Default off: PSX_KEEP_CALL_ARGS must opt in, so the shipped boot keeps the
 * historical full restore. Latched once. */
static int s_keep_call_args = -1;

int psx_guest_call_keep_args_enabled(void)
{
    if (s_keep_call_args < 0) {
        const char *e = getenv("PSX_KEEP_CALL_ARGS");
        s_keep_call_args = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_keep_call_args;
}

void psx_guest_call_set_keep_args(int on)
{
    s_keep_call_args = on ? 1 : 0;
}

uint32_t psx_guest_call_resume_pc(void)
{
    /* Outermost frame first: the guest continuation of the whole nested C
     * chain, which is what a suspended thread or an EPC must point at. */
    for (int i = 0; i < g_psx_guest_call_depth; i++)
        if (s_call_resume[i] != 0u) return s_call_resume[i];
    return 0;
}

void psx_guest_call_stats(PsxGuestCallStats* out)
{
    if (out) *out = s_call_stats;
}

static void call_log(const char* what, uint32_t callee, uint32_t ra)
{
    if (s_call_logs >= 8) return;
    s_call_logs++;
    fprintf(stderr, "psx_guest_call: %s callee 0x%08X caller ra 0x%08X\n",
            what, callee, ra);
    fflush(stderr);
}

/* A guest stack pointer is plausible when it is aligned and inside live RAM
 * with room for the argument area below it. */
static int call_sp_plausible(uint32_t sp)
{
    uint32_t off;
    if (sp == 0u || (sp & 3u) != 0u) return 0;
    if (!psx_ram_resolve(sp, 4u, &off)) return 0;
    return off + 16u + PSX_HOST_CALL_ARG_BYTES <= psx_ram_live_bytes();
}

int psx_guest_call_enter(uint32_t caller_ra, uint32_t sp)
{
    uint32_t pc = 0;
    if (!call_sp_plausible(sp)) {
        /* No usable guest stack: the callee's frame has nowhere to go and an
         * ISR taken here would save a non-stack $sp. Counted, not refused (the
         * call may still be a leaf that never touches the stack). */
        s_call_stats.bad_sp++;
        call_log("bad guest sp:", 0, sp);
    }
    if (psx_guest_pc_plausible(caller_ra)) pc = caller_ra;
    else pc = psx_guest_call_resume_pc();   /* nested call: inherit the outer frame */
    if (pc == 0u) {
        /* No guest continuation at all (the caller is host code entered without
         * a guest $ra). Count it: an interrupt or thread switch here still has
         * to fall back to the legacy sentinel path. */
        s_call_stats.bad_ra++;
        call_log("no guest continuation:", 0, caller_ra);
    }
    if (g_psx_guest_call_depth >= PSX_GUEST_CALL_MAX_DEPTH) return 0;
    s_call_resume[g_psx_guest_call_depth] = pc;
    s_sentinel[g_psx_guest_call_depth] = 0u;
    g_psx_guest_call_depth++;
    return 1;
}

void psx_guest_call_leave(int pushed)
{
    if (pushed && g_psx_guest_call_depth > 0) {
        g_psx_guest_call_depth--;
        s_call_resume[g_psx_guest_call_depth] = 0;
        s_sentinel[g_psx_guest_call_depth] = 0;
    }
}

void psx_guest_call_note_return(int returned)
{
    s_call_stats.calls++;
    if (returned) s_call_stats.returns++;
    else s_call_stats.non_returning++;
}

void psx_guest_call_note_suspended(void)
{
    s_call_stats.non_returning++;
}

void psx_guest_call_note_irq(void)
{
    s_call_stats.irq_in_call++;
}

void psx_guest_call_note_boundary(void) { s_boundaries++; }
uint64_t psx_guest_call_boundary_count(void) { return s_boundaries; }

uint32_t psx_guest_call_sentinel(void)
{
    return g_psx_guest_call_depth > 0 ? s_sentinel[g_psx_guest_call_depth - 1] : 0u;
}

uint32_t g_isr_resume_src = 0;

/* ── PSX_BIOS_TRACE=<path>: the BIOS-vector host calls, both directions ─────
 *
 * A hybrid C body reaches the kernel through we_bios_tail(), which calls
 * psx_mod_call_guest(cpu, 0xA0|0xB0|0xC0, ...) with the function index in $t1.
 * That funnel is the only place both halves of a kernel call are visible: the
 * four arguments the C body published, whether the dispatch came back, and the
 * kernel's return value in $v0. GUEST-side calls to the same vector (a `jal`
 * from recompiled/interpreter code) do NOT pass here, so a comparison run swaps
 * the body onto the guest side (WE_HYBRID_SKIP) - then the C wrapper is still
 * what forwards, and both streams carry the same shape.
 *
 * Every line also samples the kernel's EvCB table (`*(0x120)`, 22 entries of
 * 0x1C): OpenEvent (B0h 08h) is supposed to append a descriptor there, so
 * "n" before/after answers whether the kernel allocated, and `match` says
 * whether the (class, spec) pair this call asked for is present afterwards.
 * Inert when unset (one getenv per process). */
static FILE *s_bios_trace;
static int   s_bios_trace_init;
static int   s_bios_trace_lines;

static FILE *bios_trace_file(void)
{
    if (!s_bios_trace_init) {
        const char *e = getenv("PSX_BIOS_TRACE");
        s_bios_trace_init = 1;
        if (e && *e) s_bios_trace = fopen(e, "w");
    }
    return s_bios_trace;
}

/* (class, spec) of the kernel's EvCB table: entries used, and the index of the
 * requested pair (-1 = absent), with that entry's status word. */
static void bios_trace_evcb(uint32_t want_class, uint32_t want_spec,
                            int *used, int *match, uint32_t *status)
{
    *used = 0;
    *match = -1;
    *status = 0;
    const uint32_t base = psx_mod_read_word(0x120u);
    if (base == 0u) return;
    const uint32_t off = base & 0x1FFFFFu;
    if (off + 22u * 0x1Cu >= psx_ram_live_bytes()) return;
    for (int i = 0; i < 22; i++) {
        const uint32_t e = 0x80000000u + off + (uint32_t)i * 0x1Cu;
        const uint32_t cls = psx_mod_read_word(e);
        if (cls != 0u) (*used)++;
        if (cls == want_class && psx_mod_read_word(e + 8u) == want_spec) {
            *match = i;
            *status = psx_mod_read_word(e + 4u);
        }
    }
}

static void bios_trace_call(const char *half, uint32_t vec, uint32_t index,
                            uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t caller_ra, uint32_t sp, uint32_t ret,
                            int came_back)
{
    FILE *f = bios_trace_file();
    if (!f || s_bios_trace_lines >= 400) return;
    int used = 0, match = -1;
    uint32_t status = 0;
    bios_trace_evcb(a0, a1, &used, &match, &status);
    if (came_back < 0) {
        fprintf(f, "%s vec=%02X fn=%02X a0=%08X a1=%08X a2=%08X a3=%08X "
                   "idx_reg=%02X caller_ra=%08X sp=%08X cyc=%llu "
                   "evcb_used=%d match=%d\n",
                half, vec, index, a0, a1, a2, a3, index, caller_ra, sp,
                (unsigned long long)psx_get_cycle_count(), used, match);
    } else {
        fprintf(f, "%s vec=%02X fn=%02X ret=%08X came_back=%d evcb_used=%d "
                   "match=%d status=%08X cyc=%llu\n",
                half, vec, index, ret, came_back, used, match, status,
                (unsigned long long)psx_get_cycle_count());
    }
    fflush(f);
    s_bios_trace_lines++;
}

/* ── PSX_CALL_TRACE=<path>: the transient argument chain, both sides ────────
 *
 * The wrapper publishes a call's $a0..$a3 and then restores the caller's 32
 * registers, so "what the callee left in $a0..$a3" is invisible from outside.
 * This logs it: the caller's words at entry, the words the C site published,
 * and the CALLEE's words before they are restored (docs/INIT_EXIT3.md 12).
 * PSX_CALL_TRACE_PC=<hex,hex,...> keeps only calls to those targets (the card
 * stubs at 0x8004FEA4 / 0x8004FEB4 / 0x8003867C), so a boot log stays small.
 * Inert when unset (one getenv per process). */
static FILE *s_call_trace;
static int   s_call_trace_init;
static int   s_call_trace_lines;
static uint32_t s_call_trace_pc[8];
static int   s_call_trace_pc_n;

static void call_trace_filter(void)
{
    const char *e = getenv("PSX_CALL_TRACE_PC");
    if (!e) return;
    while (*e && s_call_trace_pc_n < 8) {
        char *end = 0;
        unsigned long v = strtoul(e, &end, 0);
        if (end == e) break;
        s_call_trace_pc[s_call_trace_pc_n++] = (uint32_t)v;
        e = (*end == ',') ? end + 1 : end;
        while (*e == ' ' || *e == ',') e++;
    }
}

static FILE *call_trace_file(void)
{
    if (!s_call_trace_init) {
        const char *e = getenv("PSX_CALL_TRACE");
        s_call_trace_init = 1;
        if (e && *e) {
            s_call_trace = fopen(e, "w");
            call_trace_filter();
        }
    }
    return s_call_trace;
}

static int call_trace_wanted(uint32_t function)
{
    if (s_call_trace_pc_n == 0) return 1;
    for (int i = 0; i < s_call_trace_pc_n; i++)
        if (s_call_trace_pc[i] == function) return 1;
    return 0;
}

void psx_guest_call_note_isr_return(int ok, uint32_t bad_target, uint32_t used)
{
    if (ok) {
        s_call_stats.isr_returns++;
        return;
    }
    s_call_stats.isr_bad_target++;
    if (s_call_logs < 8) {
        s_call_logs++;
        fprintf(stderr,
                "psx_guest_call: ISR return target 0x%08X is not a guest PC; resumed at 0x%08X "
                "(src %u, %s)\n",
                bad_target, used, (unsigned)g_isr_resume_src,
                used ? "substituted" : "nothing usable");
        fflush(stderr);
    }
}

uint32_t psx_mod_call_guest_ex(struct CPUState* cpu, uint32_t function,
                               uint32_t return_address, uint32_t a0, uint32_t a1,
                               uint32_t a2, uint32_t a3, int* returned) {
    uint32_t regs[32];
    const uint32_t pc = cpu->pc, hi = cpu->hi, lo = cpu->lo;
    memcpy(regs, cpu->gpr, sizeof regs);

    const int bios_trace = (function == 0xA0u || function == 0xB0u ||
                            function == 0xC0u) && bios_trace_file() != NULL;
    const uint32_t bios_index = cpu->gpr[9];
    const uint32_t bios_ra = regs[31];
    const uint32_t bios_sp = cpu->gpr[29];
    if (bios_trace)
        bios_trace_call("enter", function, bios_index, a0, a1, a2, a3,
                        bios_ra, bios_sp, 0u, -1);
    const int trace = call_trace_file() != NULL && call_trace_wanted(function);
    if (trace && s_call_trace_lines < 4000) {
        fprintf(s_call_trace,
                "enter tgt=%08X ra=%08X idx=%02X depth=%d caller_a0=%08X a1=%08X "
                "a2=%08X a3=%08X pub_a0=%08X a1=%08X a2=%08X a3=%08X sp=%08X cyc=%llu\n",
                function, bios_ra, bios_index, g_psx_guest_call_depth,
                regs[4], regs[5], regs[6], regs[7], a0, a1, a2, a3, bios_sp,
                (unsigned long long)psx_get_cycle_count());
        fflush(s_call_trace);
        s_call_trace_lines++;
    }

    /* Refuse a call whose target is not a guest code address (a null function
     * pointer in decompiled C reaches here as we_gcall(0)). Dispatching it would
     * be a dispatch miss at 0; returning 0 keeps the caller alive and the event
     * measurable. */
    if (!psx_guest_pc_plausible(function)) {
        s_call_stats.bad_ra++;
        call_log("call target is not a guest PC:", function, regs[31]);
        if (returned) *returned = 0;
        return 0;
    }

    const int pushed = psx_guest_call_enter(regs[31], cpu->gpr[29]);

    /* Reserve the callee's frame (see psx_guest_call.h). The C body never ran
     * its own prologue, so $sp is the caller's frame top: without this the
     * callee's `addiu sp,sp,-N` and any ISR taken inside the call overwrite the
     * live caller frame. The outgoing argument area moves with it so 16($sp)
     * still holds the stack-passed arguments. */
    const uint32_t sp0 = cpu->gpr[29];
    uint32_t arg[PSX_HOST_CALL_ARG_BYTES / 4u];
    const int framed = call_sp_plausible(sp0);
    if (framed) {
        for (uint32_t i = 0; i < PSX_HOST_CALL_ARG_BYTES / 4u; i++)
            arg[i] = psx_mod_read_word(sp0 + 16u + 4u * i);
        cpu->gpr[29] = sp0 - PSX_HOST_CALL_FRAME;
        for (uint32_t i = 0; i < PSX_HOST_CALL_ARG_BYTES / 4u; i++)
            psx_mod_write_word(cpu->gpr[29] + 16u + 4u * i, arg[i]);
    }
    cpu->gpr[4] = a0;
    cpu->gpr[5] = a1;
    cpu->gpr[6] = a2;
    cpu->gpr[7] = a3;
    cpu->gpr[31] = return_address;
    if (pushed && g_psx_guest_call_depth > 0)
        s_sentinel[g_psx_guest_call_depth - 1] = return_address;
    const uint64_t boundaries_before = s_boundaries;
    if (g_psx_static_guest_mode)
        psx_static_guest_note(PSX_SG_FUNNEL_MOD_CALL_GUEST, function, regs[31]);
    psx_snapshot_host_call_begin();
    psx_dispatch_call(cpu, function, return_address);
    /* Positive return test: the dispatch reported that it reached this call's
     * return address (psx_guest_call_note_boundary). $ra alone is NOT a test -
     * an interrupt delivered at the boundary can leave the handler's registers
     * (or a thread's TCB context) in place, which made ordinary returning calls
     * look non-returning (A0h vector, DMA start). */
    const int came_back = (s_boundaries != boundaries_before) ||
                          (cpu->gpr[31] == return_address);
    const uint32_t result = cpu->gpr[2];
    const uint32_t callee_a0 = cpu->gpr[4], callee_a1 = cpu->gpr[5];
    const uint32_t callee_a2 = cpu->gpr[6], callee_a3 = cpu->gpr[7];
    if (framed) cpu->gpr[29] = sp0;
    memcpy(cpu->gpr, regs, sizeof regs);
    /* Transient-argument contract (psx_guest_call.h): $a0..$a3 are caller-saved,
     * so the guest's next `jal` inherits what the callee left. Off = historical
     * full restore, bit-for-bit. */
    if (psx_guest_call_keep_args_enabled()) {
        cpu->gpr[4] = callee_a0;
        cpu->gpr[5] = callee_a1;
        cpu->gpr[6] = callee_a2;
        cpu->gpr[7] = callee_a3;
    }
    cpu->pc = pc;
    cpu->hi = hi;
    cpu->lo = lo;
    psx_snapshot_host_call_end();
    psx_guest_call_leave(pushed);
    psx_guest_call_note_return(came_back);
    if (!came_back) call_log("non-returning call:", function, regs[31]);
    if (trace && s_call_trace_lines < 4000) {
        fprintf(s_call_trace,
                "leave tgt=%08X ret=%08X came_back=%d callee_a0=%08X a1=%08X "
                "a2=%08X a3=%08X kept=%d cyc=%llu\n",
                function, result, came_back ? 1 : 0,
                callee_a0, callee_a1, callee_a2, callee_a3,
                psx_guest_call_keep_args_enabled(),
                (unsigned long long)psx_get_cycle_count());
        fflush(s_call_trace);
        s_call_trace_lines++;
    }
    if (bios_trace)
        bios_trace_call("exit", function, bios_index, a0, a1, a2, a3,
                        bios_ra, bios_sp, result, came_back ? 1 : 0);
    if (returned) *returned = came_back;
    return result;
}

uint32_t psx_mod_call_guest(struct CPUState* cpu, uint32_t function,
                            uint32_t return_address, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3) {
    return psx_mod_call_guest_ex(cpu, function, return_address, a0, a1, a2, a3, 0);
}

/* The caller's in-flight timing state: deadlines are absolute guest cycles
 * and the load pipeline describes the instructions around the call, so an
 * uncharged callee must leave them as they were. */
typedef struct CallerTiming {
    uint64_t muldiv_ts_done, gte_ts_done;
    uint8_t  read_absorb[33];
    uint8_t  read_absorb_which, read_fudge, ld_which_t;
    uint32_t ld_absorb;
} CallerTiming;

static void timing_save(CallerTiming* t, const struct CPUState* cpu) {
    t->muldiv_ts_done = cpu->muldiv_ts_done;
    t->gte_ts_done = cpu->gte_ts_done;
    memcpy(t->read_absorb, cpu->read_absorb, sizeof t->read_absorb);
    t->read_absorb_which = cpu->read_absorb_which;
    t->read_fudge = cpu->read_fudge;
    t->ld_which_t = cpu->ld_which_t;
    t->ld_absorb = cpu->ld_absorb;
}

static void timing_restore(struct CPUState* cpu, const CallerTiming* t) {
    cpu->muldiv_ts_done = t->muldiv_ts_done;
    cpu->gte_ts_done = t->gte_ts_done;
    memcpy(cpu->read_absorb, t->read_absorb, sizeof t->read_absorb);
    cpu->read_absorb_which = t->read_absorb_which;
    cpu->read_fudge = t->read_fudge;
    cpu->ld_which_t = t->ld_which_t;
    cpu->ld_absorb = t->ld_absorb;
}

uint32_t psx_mod_call_guest_uncharged(struct CPUState* cpu, uint32_t function,
                                      uint32_t return_address, uint32_t a0,
                                      uint32_t a1, uint32_t a2, uint32_t a3,
                                      uint32_t budget_cycles, int* charged) {
    PsxCycleFreeze save;
    CallerTiming timing;
    timing_save(&timing, cpu);
    const int frozen = psx_cycle_uncharged_begin(&save, budget_cycles);
    const uint32_t result =
        psx_mod_call_guest(cpu, function, return_address, a0, a1, a2, a3);
    const int uncharged = frozen ? psx_cycle_uncharged_end(&save) : 1;
    if (frozen && uncharged) timing_restore(cpu, &timing);
    if (charged) *charged = !uncharged;
    return result;
}

int psx_mod_dma_write_ram(uint32_t address, const void* data, uint32_t bytes,
                          int lba) {
    return dma_host_cdrom_write(address, (const uint8_t*)data, bytes, lba);
}

static int ram_span(uint32_t address, uint32_t bytes) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    const uint32_t live = psx_ram_live_bytes();
    return address < 0xC0000000u && phys < live && bytes <= live - phys;
}

int psx_mod_host_write_ram(uint32_t address, const void* data, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    if ((!p && bytes) || !ram_span(address, bytes)) return 0;
    while (bytes && (address & 3u)) { psx_host_write_byte(address++, *p++); --bytes; }
    for (; bytes >= 4; address += 4, p += 4, bytes -= 4) {
        uint32_t word;
        memcpy(&word, p, 4);
        psx_host_write_word(address, word);
    }
    while (bytes--) psx_host_write_byte(address++, *p++);
    return 1;
}

#define SPU_RAM_BYTES 0x80000u
#define SPU_TRANSFER_ADDR 0x1F801DA6u
#define SPU_CONTROL 0x1F801DAAu

int psx_mod_spu_upload(uint32_t spu_address, uint32_t guest_source,
                       uint32_t bytes, int stop_after) {
    if ((spu_address & 7u) || (guest_source & 3u) || (bytes & 3u) ||
        spu_address >= SPU_RAM_BYTES || bytes > SPU_RAM_BYTES ||
        !ram_span(guest_source, bytes))
        return 0;
    spu_write(SPU_TRANSFER_ADDR, spu_address >> 3);
    const uint16_t control = spu_ctrl_read();
    spu_write(SPU_CONTROL, (control & ~0x30u) | 0x20u);
    for (uint32_t i = 0; i < bytes; i += 4u) spu_dma_write(psx_mod_read_word(guest_source + i));
    if (stop_after) spu_write(SPU_CONTROL, control & ~0x30u);
    return 1;
}

int psx_mod_psyq_load_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t guest_source) {
    if (!w || w > 1024u || !h || h > 512u || x >= 1024u || y >= 512u ||
        (guest_source & 3u))
        return 0;
    const uint32_t words = (w * h + 1u) / 2u;
    if (!ram_span(guest_source, words * 4u)) return 0;
    gpu_set_gp0_source(guest_source);
    gpu_write_gp1(0x04000000u);
    gpu_write_gp0(0x01000000u);
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(x | (y << 16));
    gpu_write_gp0(w | (h << 16));
    for (uint32_t i = 0; i < words; i++) {
        gpu_set_gp0_source(guest_source + 4u * i);
        gpu_write_gp0(psx_mod_read_word(guest_source + 4u * i));
    }
    if (words >= 16u) gpu_write_gp1(0x04000002u);
    return 1;
}
