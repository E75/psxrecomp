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
#include "psx_static_guest.h"
#include "spu.h"
#include <stdio.h>
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
    if (framed) cpu->gpr[29] = sp0;
    memcpy(cpu->gpr, regs, sizeof regs);
    cpu->pc = pc;
    cpu->hi = hi;
    cpu->lo = lo;
    psx_snapshot_host_call_end();
    psx_guest_call_leave(pushed);
    psx_guest_call_note_return(came_back);
    if (!came_back) call_log("non-returning call:", function, regs[31]);
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
