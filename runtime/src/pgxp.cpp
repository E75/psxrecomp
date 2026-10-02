/* pgxp.cpp — PGXP value-propagation engine (docs/ENHANCEMENTS.md G1.2/G1.3).
 *
 * CLEAN-ROOM implementation of the publicly documented PGXP technique
 * (psx-spx GTE docs + public design write-ups + our own G1 measurements).
 * The vendored duckstation/ (CC BY-NC-ND) and beetle-psx/ (GPL) trees are
 * black-box behavioral oracles only — no code from them appears here.
 *
 * Model
 * -----
 * Every 32-bit word of guest RAM/scratchpad, every GPR (plus HI/LO), and
 * every GTE data register owns a shadow slot recording the sub-pixel screen
 * position that word carries (16.16 X/Y + projected SZ depth), the exact
 * guest word it describes (`value`), and per-half validity flags. RTPS/RTPT
 * fill the SXY shadow FIFO with the pre-truncation projection; the
 * psx_pgxp_* hooks copy shadows along with the data (loads, stores, COP2
 * transfers, and — in cpu-mode — the arithmetic games use to repack vertex
 * halves); the GPU asks for the precise position of a GP0 vertex word by the
 * packet's RAM address.
 *
 * The single safety invariant: a shadow is only ever BELIEVED after
 * validation against the actual guest word it claims to describe. Anything
 * that writes guest state without a hook (DMA, memcpy loaders, un-hooked
 * instructions) simply leaves a stale shadow behind, and the next validation
 * drops it. We never model side effects — overwrite and validate only. The
 * one accepted hole (shared with the reference implementations): an untracked
 * writer storing the byte-identical word keeps the shadow alive, which is
 * harmless because the position it describes is still that word.
 *
 * Everything here is host-only and visual-only: guest-visible state is never
 * read back from shadows, shadows are dropped on savestate/rewind, and the
 * speculative native-validation bracket suppresses all recording.
 */

#include "pgxp.h"
#include "pgxp_hooks.h"
#include "cpu_state.h"
#include "psx_memory.h"

#include <cstdlib>
#include <cstring>

/* gte.cpp — position-cache fallback tier (ambiguity-gated, G1.4 exact table) */
extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16);

/* ------------------------------------------------------------------------- */
/* Shadow storage                                                             */
/* ------------------------------------------------------------------------- */

enum {
    PGXP_F_VX = 1u << 0,   /* low half  (screen X) tracked                    */
    PGXP_F_VY = 1u << 1,   /* high half (screen Y) tracked                    */
    PGXP_F_VZ = 1u << 2,   /* projected depth rode along intact               */
    PGXP_F_VXY = PGXP_F_VX | PGXP_F_VY,
};

struct PGXPValue {
    int32_t  x16, y16;   /* sub-pixel screen coords, 16.16                    */
    uint16_t z;          /* projected SZ depth (perspective source), 0 = none */
    uint16_t flags;
    uint32_t value;      /* the guest word this shadow describes              */
    uint32_t gen;        /* valid iff == s_gen (O(1) invalidate-all)          */
};

/* Shadow covers the host RAM backing so the opt-in 8 MB map tracks its high
 * banks; retail sessions only ever touch the low 2 MiB of it. */
#define PGXP_RAM_WORDS     (PSX_MAIN_RAM_BACKING_BYTES >> 2)
#define PGXP_SCRATCH_WORDS (0x400u >> 2)      /* 1 KB scratchpad              */
#define PGXP_REG_HI        32
#define PGXP_REG_LO        33

static PGXPValue *s_ram = nullptr;            /* lazily allocated, ~40 MB VA  */
static PGXPValue  s_scratch[PGXP_SCRATCH_WORDS];
static PGXPValue  s_gpr[34];                  /* 32 GPRs + HI + LO            */
static PGXPValue  s_gte[32];                  /* GTE data registers           */

static uint32_t s_gen = 1;
static int      s_enabled = 0;
static int      s_cpu_mode = 0;
static float    s_tolerance = 0.5f;   /* user-validated seam clamp (G1.10) */
static uint32_t s_suppress = 0;
static int      s_deferred_invalidate = 0;

/* Single hot-path gate for every hook. */
static int g_pgxp_active = 0;
static inline void recompute_active(void) {
    g_pgxp_active = (s_enabled && s_suppress == 0) ? 1 : 0;
}

static PGXPStats s_stats;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

static void ck_wrapped(void);

extern "C" void pgxp_invalidate_all(void) {
    if (s_suppress != 0) { s_deferred_invalidate = 1; return; }
    if (++s_gen == 0) {
        ck_wrapped();
        /* generation wrapped: physically clear so stale slots can't revive */
        if (s_ram) std::memset(s_ram, 0, PGXP_RAM_WORDS * sizeof(PGXPValue));
        std::memset(s_scratch, 0, sizeof(s_scratch));
        std::memset(s_gpr, 0, sizeof(s_gpr));
        std::memset(s_gte, 0, sizeof(s_gte));
        s_gen = 1;
    }
}

extern "C" void pgxp_set_enabled(int enabled) {
    int resized = 0;
    if (enabled && !s_ram) {
        s_ram = (PGXPValue *)std::calloc(PGXP_RAM_WORDS, sizeof(PGXPValue));
        if (s_ram)
            resized = 1;
        else
            enabled = 0;                      /* fail closed: stay faithful   */
    }
    /* Re-applying configuration must not invalidate every live shadow. */
    if (s_enabled == (enabled ? 1 : 0) && !resized)
        return;
    s_enabled = enabled ? 1 : 0;
    pgxp_invalidate_all();
    recompute_active();
}

extern "C" int pgxp_enabled(void) { return s_enabled; }

extern "C" void pgxp_set_cpu_mode(int enabled) { s_cpu_mode = enabled ? 1 : 0; }
extern "C" int  pgxp_cpu_mode(void) { return s_cpu_mode; }

extern "C" void  pgxp_set_tolerance(float pixels) { s_tolerance = pixels; }
extern "C" float pgxp_tolerance(void) { return s_tolerance; }

extern "C" void pgxp_suppress_begin(void) {
    ++s_suppress;
    recompute_active();
}

extern "C" void pgxp_suppress_end(void) {
    if (s_suppress != 0 && --s_suppress == 0) {
        if (s_deferred_invalidate) {
            s_deferred_invalidate = 0;
            pgxp_invalidate_all();
        }
        recompute_active();
    }
}

extern "C" void pgxp_get_stats(PGXPStats *out) {
    if (out) *out = s_stats;
}

/* ------------------------------------------------------------------------- */
/* Address mapping + validation                                               */
/* ------------------------------------------------------------------------- */

/* Guest address -> shadow slot, or NULL for BIOS/MMIO/KSEG2 (untrackable). */
static inline PGXPValue *pgxp_ptr(uint32_t addr) {
    uint32_t m = addr & 0x1FFFFFFFu;
    if (m < PSX_MAIN_RAM_WINDOW_BYTES)         /* RAM + its mirrors (live map) */
        return s_ram ? &s_ram[psx_ram_canonical_offset(m) >> 2] : nullptr;
    if ((m & 0xFFFFFC00u) == 0x1F800000u)      /* scratchpad                  */
        return &s_scratch[(m & 0x3FCu) >> 2];
    return nullptr;
}

/* ------------------------------------------------------------------------- */
/* Checkpoint (render passes)                                                 */
/* ------------------------------------------------------------------------- */

/* A render pass (render_pass.c) runs guest draw code on a sandboxed machine
 * and puts RAM, scratchpad and registers back with raw copies afterwards. The
 * shadows must roll back with them: otherwise they describe the words the
 * pass wrote (interpolated vertices), and the live frame's packets - consumed
 * after the passes - fail validation. Copying the shadow arrays whole per
 * pass would cost tens of MB; instead every RAM / scratchpad slot a pass
 * mutates is journaled on its first write, and register shadows (small) are
 * copied whole. A journal that cannot grow, or a generation wrap during the
 * pass, fails closed: rollback invalidates everything. */
struct PGXPJournalEntry {
    PGXPValue *slot;
    PGXPValue old;
};

static uint32_t          s_ck_depth = 0;
static uint8_t          *s_ck_bits = nullptr;     /* one bit per shadow slot */
static PGXPValue        *s_ck_ram = nullptr;      /* s_ram the bits index    */
static PGXPJournalEntry *s_ck_log = nullptr;
static size_t            s_ck_n = 0, s_ck_cap = 0;
static int               s_ck_lossy = 0;
static PGXPValue         s_ck_gpr[34], s_ck_gte[32];
static uint32_t          s_ck_gen = 0, s_ck_suppress = 0;
static int               s_ck_deferred = 0;

static inline size_t ck_index(const PGXPValue *pv) {
    if (pv >= s_scratch && pv < s_scratch + PGXP_SCRATCH_WORDS)
        return (size_t)PGXP_RAM_WORDS + (size_t)(pv - s_scratch);
    return (size_t)(pv - s_ram);
}

/* Journal a RAM / scratchpad slot before its first mutation in a pass. */
static inline void ck_note(PGXPValue *pv) {
    if (s_ck_depth == 0 || !pv) return;
    if (s_ram != s_ck_ram && !(pv >= s_scratch &&
                               pv < s_scratch + PGXP_SCRATCH_WORDS)) {
        s_ck_lossy = 1;                        /* shadow RAM appeared mid-pass */
        return;
    }
    size_t i = ck_index(pv);
    uint8_t bit = (uint8_t)(1u << (i & 7u));
    if (s_ck_bits[i >> 3] & bit) return;
    if (s_ck_n == s_ck_cap) {
        size_t cap = s_ck_cap ? s_ck_cap * 2 : 65536;
        PGXPJournalEntry *p = (PGXPJournalEntry *)std::realloc(
            s_ck_log, cap * sizeof *p);
        if (!p) { s_ck_lossy = 1; return; }
        s_ck_log = p;
        s_ck_cap = cap;
    }
    s_ck_bits[i >> 3] |= bit;
    s_ck_log[s_ck_n].slot = pv;
    s_ck_log[s_ck_n].old = *pv;
    s_ck_n++;
}

static void ck_wrapped(void) {
    if (s_ck_depth != 0) s_ck_lossy = 1;       /* shadows were cleared        */
}

static inline PGXPValue *pgxp_ptr_w(uint32_t addr) {
    PGXPValue *pv = pgxp_ptr(addr);
    ck_note(pv);
    return pv;
}

extern "C" void pgxp_checkpoint_begin(void) {
    if (s_ck_depth++ != 0) return;             /* the outermost pass journals */
    if (!s_ck_bits) {
        s_ck_bits = (uint8_t *)std::calloc(
            ((size_t)PGXP_RAM_WORDS + PGXP_SCRATCH_WORDS + 7u) / 8u, 1);
        if (!s_ck_bits) s_ck_lossy = 1;
    }
    s_ck_ram = s_ram;
    s_ck_n = 0;
    std::memcpy(s_ck_gpr, s_gpr, sizeof s_gpr);
    std::memcpy(s_ck_gte, s_gte, sizeof s_gte);
    s_ck_gen = s_gen;
    s_ck_suppress = s_suppress;
    s_ck_deferred = s_deferred_invalidate;
}

extern "C" void pgxp_checkpoint_rollback(void) {
    if (s_ck_depth == 0) return;
    if (--s_ck_depth != 0) return;
    for (size_t i = s_ck_n; i-- > 0;) {
        PGXPJournalEntry *e = &s_ck_log[i];
        *e->slot = e->old;
        size_t k = ck_index(e->slot);
        s_ck_bits[k >> 3] &= (uint8_t)~(1u << (k & 7u));
    }
    s_ck_n = 0;
    std::memcpy(s_gpr, s_ck_gpr, sizeof s_gpr);
    std::memcpy(s_gte, s_ck_gte, sizeof s_gte);
    s_gen = s_ck_gen;
    /* A watchdog abort can leave a suppress bracket open: the machine it
     * interrupted is gone, so its bracket is too. */
    s_suppress = s_ck_suppress;
    s_deferred_invalidate = s_ck_deferred;
    recompute_active();
    if (s_ck_lossy || s_ram != s_ck_ram) {
        s_ck_lossy = 0;
        pgxp_invalidate_all();
    }
}

static inline int pv_live(const PGXPValue *pv) {
    return pv && pv->gen == s_gen && (pv->flags & PGXP_F_VXY) != 0;
}

/* Drop whichever tracked halves no longer match the actual guest word; the
 * depth belongs to the whole vertex, so any half going stale kills it too. */
static inline void pv_validate(PGXPValue *pv, uint32_t actual) {
    if (pv->gen != s_gen) return;
    uint32_t diff = pv->value ^ actual;
    if ((pv->flags & PGXP_F_VX) && (diff & 0x0000FFFFu))
        pv->flags &= (uint16_t)~(PGXP_F_VX | PGXP_F_VZ);
    if ((pv->flags & PGXP_F_VY) && (diff & 0xFFFF0000u))
        pv->flags &= (uint16_t)~(PGXP_F_VY | PGXP_F_VZ);
    pv->value = actual;
}

/* Mark a slot as "known word, no precision" — keeps `value` current so later
 * half-merges stay keyed correctly. */
static inline void pv_reset(PGXPValue *pv, uint32_t value) {
    pv->x16 = 0; pv->y16 = 0; pv->z = 0;
    pv->flags = 0;
    pv->value = value;
    pv->gen = s_gen;
}

static inline void pv_kill(PGXPValue *pv) { pv->gen = 0; }

/* ------------------------------------------------------------------------- */
/* Instruction field helpers                                                  */
/* ------------------------------------------------------------------------- */

static inline uint32_t f_op(uint32_t i)    { return i >> 26; }
static inline uint32_t f_rs(uint32_t i)    { return (i >> 21) & 31u; }
static inline uint32_t f_rt(uint32_t i)    { return (i >> 16) & 31u; }
static inline uint32_t f_rd(uint32_t i)    { return (i >> 11) & 31u; }
static inline uint32_t f_shamt(uint32_t i) { return (i >> 6) & 31u; }
static inline uint32_t f_funct(uint32_t i) { return i & 63u; }
static inline int32_t  f_simm(uint32_t i)  { return (int32_t)(int16_t)(i & 0xFFFFu); }

/* ------------------------------------------------------------------------- */
/* Memory-mode hooks: loads / stores                                          */
/* ------------------------------------------------------------------------- */

/* SXY2 and SXYP are one register seen at two addresses: whichever the hook
 * just filled, the other describes the same word. */
static inline void gte_sxy2_mirror(uint32_t reg) {
    if (reg == 14)      s_gte[15] = s_gte[14];
    else if (reg == 15) s_gte[14] = s_gte[15];
}

extern "C" void psx_pgxp_load(struct CPUState *cpu, uint32_t instr,
                              uint32_t addr, uint32_t value) {
    (void)cpu;
    if (!g_pgxp_active) return;
    uint32_t rt = f_rt(instr);
    if (rt == 0) return;
    PGXPValue *dst = &s_gpr[rt];

    switch (f_op(instr)) {
    case 0x23: {                               /* LW                          */
        PGXPValue *src = pgxp_ptr_w(addr);     /* validation may mutate it    */
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        return;
    }
    case 0x21:                                 /* LH                          */
    case 0x25: {                               /* LHU                         */
        PGXPValue *src = pgxp_ptr(addr);
        int hi_half = (addr >> 1) & 1;
        pv_reset(dst, value);
        /* The high half of the extended GPR is a known-exact constant
         * (0/-1 for LH, 0 for LHU): track it so re-packing via sll/or in
         * cpu-mode keeps working. */
        dst->y16 = ((int32_t)value >> 16) << 16;
        dst->flags = PGXP_F_VY;
        if (src && src->gen == s_gen) {
            uint32_t actual_half = (value & 0xFFFFu) << (hi_half ? 16 : 0);
            uint32_t mask = hi_half ? 0xFFFF0000u : 0x0000FFFFu;
            uint16_t want = hi_half ? PGXP_F_VY : PGXP_F_VX;
            if ((src->flags & want) && ((src->value ^ actual_half) & mask) == 0) {
                dst->x16 = hi_half ? src->y16 : src->x16;
                dst->flags |= PGXP_F_VX;
            }
        }
        return;
    }
    default:                                   /* LB/LBU/LWL/LWR: untrackable */
        pv_reset(dst, value);
        return;
    }
}

extern "C" void psx_pgxp_store(struct CPUState *cpu, uint32_t instr,
                               uint32_t addr, uint32_t value) {
    (void)cpu;
    if (!g_pgxp_active) return;
    PGXPValue *dst = pgxp_ptr_w(addr);
    if (!dst) return;
    uint32_t rt = f_rt(instr);
    PGXPValue *src = (rt != 0) ? &s_gpr[rt] : nullptr;

    switch (f_op(instr)) {
    case 0x2B: {                               /* SW                          */
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        return;
    }
    case 0x29: {                               /* SH                          */
        int hi_half = (addr >> 1) & 1;
        uint32_t half = value & 0xFFFFu;
        if (dst->gen != s_gen) pv_reset(dst, half << (hi_half ? 16 : 0));
        /* Patch the stored half into the tracked word; the other half's
         * validity (if any) survives untouched. Depth never survives a
         * half-write — the vertex it described no longer exists whole. */
        if (hi_half) dst->value = (dst->value & 0x0000FFFFu) | (half << 16);
        else         dst->value = (dst->value & 0xFFFF0000u) | half;
        dst->flags &= (uint16_t)~((hi_half ? PGXP_F_VY : PGXP_F_VX) | PGXP_F_VZ);
        dst->z = 0;
        if (src && src->gen == s_gen && (src->flags & PGXP_F_VX) &&
            ((src->value ^ value) & 0xFFFFu) == 0) {
            if (hi_half) { dst->y16 = src->x16; dst->flags |= PGXP_F_VY; }
            else         { dst->x16 = src->x16; dst->flags |= PGXP_F_VX; }
        }
        return;
    }
    case 0x28: {                               /* SB                          */
        if (dst->gen != s_gen) return;         /* nothing tracked: stay dead  */
        uint32_t shift = (addr & 3u) * 8u;
        dst->value = (dst->value & ~(0xFFu << shift)) |
                     ((value & 0xFFu) << shift);
        dst->flags &= (uint16_t)~(((addr & 2u) ? PGXP_F_VY : PGXP_F_VX) | PGXP_F_VZ);
        dst->z = 0;
        return;
    }
    default:                                   /* SWL/SWR: forget the word    */
        pv_kill(dst);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Memory-mode hooks: COP2 transfers                                          */
/* ------------------------------------------------------------------------- */

extern "C" void psx_pgxp_cop2(struct CPUState *cpu, uint32_t instr,
                              uint32_t value, uint32_t addr) {
    (void)cpu;
    if (!g_pgxp_active) return;

    switch (f_op(instr)) {
    case 0x32: {                               /* LWC2: gte[rt] <- [addr]     */
        PGXPValue *src = pgxp_ptr_w(addr);     /* validation may mutate it    */
        PGXPValue *dst = &s_gte[f_rt(instr)];
        if (src && src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        gte_sxy2_mirror(f_rt(instr));
        return;
    }
    case 0x3A: {                               /* SWC2: [addr] <- gte[rt]     */
        PGXPValue *dst = pgxp_ptr_w(addr);
        if (!dst) return;
        PGXPValue *src = &s_gte[f_rt(instr)];
        if (src->gen == s_gen) {
            pv_validate(src, value);
            *dst = *src;
        } else {
            pv_reset(dst, value);
        }
        return;
    }
    case 0x12: {                               /* COP2 register transfers     */
        switch (f_rs(instr)) {
        case 0x00: {                           /* MFC2: gpr[rt] <- gte[rd]    */
            uint32_t rt = f_rt(instr);
            if (rt == 0) return;
            PGXPValue *src = &s_gte[f_rd(instr)];
            if (src->gen == s_gen) {
                pv_validate(src, value);
                s_gpr[rt] = *src;
            } else {
                pv_reset(&s_gpr[rt], value);
            }
            return;
        }
        case 0x04: {                           /* MTC2: gte[rd] <- gpr[rt]    */
            uint32_t rt = f_rt(instr);
            PGXPValue *dst = &s_gte[f_rd(instr)];
            if (rt != 0 && s_gpr[rt].gen == s_gen) {
                pv_validate(&s_gpr[rt], value);
                *dst = s_gpr[rt];
            } else {
                pv_reset(dst, value);
            }
            /* An SXYP write (rd==15) already shifted the FIFO shadows
             * (pgxp_gte_reg_written, from the GTE register write). */
            gte_sxy2_mirror(f_rd(instr));
            return;
        }
        case 0x02: {                           /* CFC2: control regs carry no
                                                  positions                   */
            uint32_t rt = f_rt(instr);
            if (rt != 0) pv_reset(&s_gpr[rt], value);
            return;
        }
        default:                               /* CTC2: nothing shadowed      */
            return;
        }
    }
    default:
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* CPU-mode hooks: arithmetic (tier 2, default off)                           */
/* ------------------------------------------------------------------------- */

/* One 16-bit operand component in 16.16: a tracked half contributes its
 * sub-pixel value; an untracked half contributes its exact integer value
 * (adding an exact offset to a tracked coordinate keeps the fraction). */
static inline int32_t comp16(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (pv && pv->gen == s_gen) {
        if (!hi_half && (pv->flags & PGXP_F_VX) &&
            ((pv->value ^ value) & 0x0000FFFFu) == 0)
            return pv->x16;
        if (hi_half && (pv->flags & PGXP_F_VY) &&
            ((pv->value ^ value) & 0xFFFF0000u) == 0)
            return pv->y16;
    }
    return ((int32_t)(int16_t)(hi_half ? (value >> 16) : value)) << 16;
}

static inline int comp_tracked(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!pv || pv->gen != s_gen) return 0;
    if (!hi_half)
        return (pv->flags & PGXP_F_VX) &&
               ((pv->value ^ value) & 0x0000FFFFu) == 0;
    return (pv->flags & PGXP_F_VY) &&
           ((pv->value ^ value) & 0xFFFF0000u) == 0;
}

/* Half-wise add/sub is only meaningful when the guest result shows no carry
 * crossed the half boundary; otherwise the halves did not combine
 * independently and the shadow must not pretend they did. */
static inline int halves_independent(uint32_t a, uint32_t b, uint32_t r, int sub) {
    uint32_t lo = sub ? ((a & 0xFFFFu) - (b & 0xFFFFu))
                      : ((a & 0xFFFFu) + (b & 0xFFFFu));
    return ((lo ^ r) & 0xFFFFu) == 0 &&
           ((((sub ? a - b : a + b)) ^ r) == 0) &&
           ((lo >> 16) == 0);                  /* no carry/borrow out of low  */
}

/* Bitwise ops (AND/OR/XOR/NOR and ANDI/ORI/XORI) move bits; they do not
 * compute coordinates. GP0 decodes only the 11-bit field of each vertex half
 * (bits 0..10 of X, 16..26 of Y), and engines pack clip / outcode bits into
 * the rest of the word after projecting (Spider-Man ORs off-screen flags into
 * bits 14/15 of each half of SXY2 before storing it). A result half whose GPU
 * field equals the field of a tracked operand half therefore still describes
 * that vertex. Carrying it is exact - no arithmetic, like a move - so it runs
 * in both tiers. The carried value keeps the shadow invariant (integer part
 * == the half read as int16): the fraction rides along, the integer becomes
 * the result half. The GPU consumer rebases onto the field it decodes. */
static inline int half_exact(const PGXPValue *pv, uint32_t value, int hi_half) {
    if (!comp_tracked(pv, value, hi_half)) return 0;
    int32_t v16 = hi_half ? pv->y16 : pv->x16;
    /* A saturated projection shadow sits beyond the clamped word; it is not
     * the value of that half and must not be carried as one. */
    return (v16 >> 16) == (int32_t)(int16_t)(hi_half ? (value >> 16) : value);
}

static inline int carry_field(PGXPValue *dst, const PGXPValue *src,
                              uint32_t sval, uint32_t result, int hi_half) {
    if (!half_exact(src, sval, hi_half)) return 0;
    uint32_t sh = hi_half ? 16u : 0u;
    if ((((sval ^ result) >> sh) & 0x7FFu) != 0) return 0;
    int32_t frac = (hi_half ? src->y16 : src->x16) & 0xFFFF;
    int32_t v = (int32_t)((uint32_t)(int32_t)(int16_t)(result >> sh) << 16) | frac;
    if (hi_half) { dst->y16 = v; dst->flags |= PGXP_F_VY; }
    else         { dst->x16 = v; dst->flags |= PGXP_F_VX; }
    return 1;
}

static inline int has_depth(const PGXPValue *pv) {
    return pv && pv->gen == s_gen && (pv->flags & PGXP_F_VZ) && pv->z != 0;
}

/* Depth belongs to a whole vertex: it survives an op only when both halves of
 * the result came from one operand that carried it, and the other operand is
 * not itself a vertex (a constant, a mask, an offset). */
static inline void keep_depth(PGXPValue *dst, const PGXPValue *p,
                              const PGXPValue *other) {
    if ((dst->flags & PGXP_F_VXY) == PGXP_F_VXY && has_depth(p) &&
        !has_depth(other)) {
        dst->z = p->z;
        dst->flags |= PGXP_F_VZ;
    }
}

static void alu_bitwise(PGXPValue *dst, const PGXPValue *a, uint32_t s1,
                        const PGXPValue *b, uint32_t s2, uint32_t result) {
    pv_reset(dst, result);
    /* Prefer the operand that is a whole vertex, so a vertex ORed with flags
     * keeps its depth even when the flag word happens to share a field. */
    const PGXPValue *p = a; uint32_t pval = s1;
    const PGXPValue *q = b; uint32_t qval = s2;
    if (!has_depth(a) && has_depth(b)) { p = b; pval = s2; q = a; qval = s1; }
    const PGXPValue *from[2] = { nullptr, nullptr };
    for (int hi = 0; hi < 2; hi++) {
        if (carry_field(dst, p, pval, result, hi))      from[hi] = p;
        else if (carry_field(dst, q, qval, result, hi)) from[hi] = q;
        else if (((hi ? result >> 16 : result) & 0xFFFFu) == 0) {
            /* An exactly-zero half is an exact coordinate (the repack
             * pattern ORs a shifted half into a zero half). */
            if (hi) { dst->y16 = 0; dst->flags |= PGXP_F_VY; }
            else    { dst->x16 = 0; dst->flags |= PGXP_F_VX; }
        }
    }
    if (from[0] && from[0] == from[1])
        keep_depth(dst, from[0], from[0] == p ? q : p);
}

static const PGXPValue kNoShadow = { 0, 0, 0, 0, 0, 0 };

/* Snapshot a source GPR shadow: the destination may alias a source (in-place
 * ops such as `or t2, t2, t9` or `addiu t0, t0, 4`), and it is reset before
 * the sources are read. */
static inline const PGXPValue *gpr_shadow(uint32_t r, PGXPValue *copy) {
    *copy = (r != 0) ? s_gpr[r] : kNoShadow;
    return copy;
}

extern "C" void psx_pgxp_alu(struct CPUState *cpu, uint32_t instr,
                             uint32_t result, uint32_t s1, uint32_t s2) {
    (void)cpu;
    if (!g_pgxp_active) return;

    uint32_t op = f_op(instr);
    PGXPValue ca, cb;

    if (op == 0) {                             /* SPECIAL                     */
        uint32_t dst_reg = f_rd(instr);
        uint32_t rs = f_rs(instr), rt = f_rt(instr);
        uint32_t funct = f_funct(instr);
        if (funct == 0x11 || funct == 0x13) {  /* MTHI / MTLO                 */
            PGXPValue *hl = &s_gpr[funct == 0x11 ? PGXP_REG_HI : PGXP_REG_LO];
            if (s_cpu_mode && rs != 0 && s_gpr[rs].gen == s_gen) {
                pv_validate(&s_gpr[rs], result);
                *hl = s_gpr[rs];
            } else pv_reset(hl, result);
            return;
        }
        if (dst_reg == 0) return;
        PGXPValue *dst = &s_gpr[dst_reg];

        switch (funct) {
        case 0x10:                             /* MFHI                        */
        case 0x12: {                           /* MFLO                        */
            PGXPValue *hl = &s_gpr[funct == 0x10 ? PGXP_REG_HI : PGXP_REG_LO];
            if (s_cpu_mode && hl->gen == s_gen) {
                pv_validate(hl, result);
                *dst = *hl;
            } else pv_reset(dst, result);
            return;
        }
        case 0x00: case 0x02: case 0x03:       /* SLL / SRL / SRA             */
        case 0x04: case 0x06: case 0x07: {     /* SLLV / SRLV / SRAV          */
            uint32_t sh = (funct < 4) ? f_shamt(instr) : (s2 & 31u);
            const PGXPValue *src = gpr_shadow(rt, &ca);
            int right = (funct & 3u) != 0;
            pv_reset(dst, result);
            if (!s_cpu_mode || sh != 16 || src->gen != s_gen)
                return;
            if (!right) {                      /* << 16: low comp -> Y        */
                if (comp_tracked(src, s1, 0)) {
                    dst->y16 = src->x16;
                    dst->flags |= PGXP_F_VY;
                }
                dst->x16 = 0;
                dst->flags |= PGXP_F_VX;       /* low half exactly zero       */
            } else {                           /* >> 16: high comp -> X       */
                if (comp_tracked(src, s1, 1)) {
                    dst->x16 = src->y16;
                    dst->flags |= PGXP_F_VX;
                }
                dst->y16 = ((int32_t)result >> 16) << 16;  /* 0 or sign fill  */
                dst->flags |= PGXP_F_VY;
            }
            return;
        }
        case 0x24: case 0x25: case 0x26: case 0x27: {  /* AND/OR/XOR/NOR     */
            const PGXPValue *a = gpr_shadow(rs, &ca);
            const PGXPValue *b = gpr_shadow(rt, &cb);
            /* OR with $zero is the MOVE idiom: the whole shadow, depth too. */
            if (funct == 0x25 && (rs == 0 || rt == 0)) {
                const PGXPValue *m = (rt == 0) ? a : b;
                if (m->gen == s_gen) {
                    *dst = *m;
                    pv_validate(dst, result);
                } else pv_reset(dst, result);
                return;
            }
            alu_bitwise(dst, a, s1, b, s2, result);
            return;
        }
        case 0x20: case 0x21:                  /* ADD / ADDU                  */
        case 0x22: case 0x23: {                /* SUB / SUBU                  */
            const PGXPValue *a = gpr_shadow(rs, &ca);
            const PGXPValue *b = gpr_shadow(rt, &cb);
            int is_sub = (funct == 0x22 || funct == 0x23);
            /* MOVE idioms first - exact, so they run in both tiers. */
            if (rt == 0 || (!is_sub && rs == 0)) {
                const PGXPValue *m = (rt == 0) ? a : b;
                if (m->gen == s_gen) {
                    *dst = *m;
                    pv_validate(dst, result);
                } else pv_reset(dst, result);
                return;
            }
            if (!s_cpu_mode) { pv_reset(dst, result); return; }
            /* add/sub: require at least one tracked component and halves
             * that combined independently (no cross-half carry). */
            if (!halves_independent(s1, s2, result, is_sub) ||
                (!comp_tracked(a, s1, 0) && !comp_tracked(a, s1, 1) &&
                 !comp_tracked(b, s2, 0) && !comp_tracked(b, s2, 1))) {
                pv_reset(dst, result);
                return;
            }
            pv_reset(dst, result);
            dst->x16 = is_sub ? comp16(a, s1, 0) - comp16(b, s2, 0)
                              : comp16(a, s1, 0) + comp16(b, s2, 0);
            dst->y16 = is_sub ? comp16(a, s1, 1) - comp16(b, s2, 1)
                              : comp16(a, s1, 1) + comp16(b, s2, 1);
            dst->flags = PGXP_F_VXY;
            /* A vertex plus / minus an offset is still that vertex. */
            if (comp_tracked(a, s1, 0) && comp_tracked(a, s1, 1))
                keep_depth(dst, a, b);
            else if (!is_sub && comp_tracked(b, s2, 0) && comp_tracked(b, s2, 1))
                keep_depth(dst, b, a);
            return;
        }
        default:                               /* SLT/SLTU/...: not a vertex  */
            pv_reset(dst, result);
            return;
        }
    }

    /* immediates */
    uint32_t dst_reg = f_rt(instr);
    if (dst_reg == 0) return;
    PGXPValue *dst = &s_gpr[dst_reg];
    const PGXPValue *a = gpr_shadow(f_rs(instr), &ca);

    switch (op) {
    case 0x0F: {                               /* LUI: both halves exact      */
        pv_reset(dst, result);
        dst->x16 = 0;
        dst->y16 = ((int32_t)result >> 16) << 16;
        dst->flags = PGXP_F_VXY;
        return;
    }
    case 0x08: case 0x09: {                    /* ADDI / ADDIU                */
        int32_t imm = f_simm(instr);
        if (imm == 0) {                        /* MOVE idiom                  */
            if (a->gen == s_gen) {
                *dst = *a;
                pv_validate(dst, result);
            } else pv_reset(dst, result);
            return;
        }
        /* A negative immediate is a subtraction of its magnitude: checking
         * carry on the sign-extended form would reject nearly every -N. */
        int neg = imm < 0;
        uint32_t mag = (uint32_t)(neg ? -imm : imm);
        if (!s_cpu_mode ||
            !halves_independent(s1, mag, result, neg) ||
            (!comp_tracked(a, s1, 0) && !comp_tracked(a, s1, 1))) {
            pv_reset(dst, result);
            return;
        }
        int32_t dx = (int32_t)(mag & 0xFFFFu) << 16;
        pv_reset(dst, result);
        dst->x16 = comp16(a, s1, 0) + (neg ? -dx : dx);
        dst->y16 = comp16(a, s1, 1);           /* no carry crossed: Y untouched */
        dst->flags = PGXP_F_VXY;
        if (comp_tracked(a, s1, 0) && comp_tracked(a, s1, 1))
            keep_depth(dst, a, nullptr);
        return;
    }
    case 0x0C: case 0x0D: case 0x0E: {         /* ANDI / ORI / XORI           */
        uint32_t imm = instr & 0xFFFFu;
        alu_bitwise(dst, a, s1, nullptr, imm, result);
        /* ORI into a zero low half writes an exact constant there (the
         * lui/ori and repack idioms). */
        if (op == 0x0D && !(dst->flags & PGXP_F_VX) && (s1 & 0xFFFFu) == 0) {
            dst->x16 = ((int32_t)(int16_t)imm) << 16;
            dst->flags |= PGXP_F_VX;
        }
        return;
    }
    default:                                   /* SLTI/SLTIU: not a vertex    */
        pv_reset(dst, result);
        return;
    }
}

extern "C" void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instr,
                                uint32_t hi, uint32_t lo,
                                uint32_t s1, uint32_t s2) {
    (void)cpu; (void)instr; (void)s1; (void)s2;
    if (!g_pgxp_active) return;
    /* Products/quotients of screen coordinates are not screen coordinates:
     * record the results as known-but-imprecise so MFHI/MFLO stay honest. */
    pv_reset(&s_gpr[PGXP_REG_HI], hi);
    pv_reset(&s_gpr[PGXP_REG_LO], lo);
}

/* ------------------------------------------------------------------------- */
/* GTE producer                                                               */
/* ------------------------------------------------------------------------- */

extern "C" void pgxp_gte_push_sxy(int32_t x16, int32_t y16, uint16_t sz3,
                                  uint32_t packed) {
    if (!g_pgxp_active) return;
    s_stats.produced++;
    s_gte[12] = s_gte[13];
    s_gte[13] = s_gte[14];
    PGXPValue *pv = &s_gte[14];
    pv->x16 = x16;
    pv->y16 = y16;
    pv->z = sz3;
    pv->flags = (uint16_t)(PGXP_F_VXY | (sz3 != 0 ? PGXP_F_VZ : 0));
    pv->value = packed;
    pv->gen = s_gen;
    s_gte[15] = *pv;                           /* SXYP mirrors SXY2           */
}

extern "C" int pgxp_get_gte_sxy(uint32_t index, int32_t *x16, int32_t *y16) {
    return pgxp_get_gte_sxy_checked(index, 0u, 0, x16, y16);
}

extern "C" int pgxp_get_gte_sxy_checked(uint32_t index, uint32_t expect,
                                        int check, int32_t *x16, int32_t *y16) {
    if (index >= 4) return 0;
    const PGXPValue *pv = &s_gte[12 + index];
    if (!pv_live(pv) || (pv->flags & PGXP_F_VXY) != PGXP_F_VXY)
        return 0;
    if (check && pv->value != expect)
        return 0;
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    return 1;
}

extern "C" void pgxp_gte_reg_written(int reg, uint32_t value) {
    /* Invalidation-class bookkeeping: runs even with the engine disarmed so
     * seeded/leftover shadows can never outlive a guest register write. Only
     * the suppression bracket skips it (the speculative pass rolls the
     * machine state back, so its writes must not stick to the shadows). */
    if (s_suppress != 0) return;
    if (reg < 0 || reg > 31) return;
    if (reg == 15) {
        /* SXYP is a FIFO push: SXY1 moves to SXY0, SXY2 to SXY1, the value
         * lands in SXY2 (psx-spx GTE "SXYP"). Engines draw quads from a
         * projected-vertex table this way - load three corners, NCLIP, push
         * the fourth through SXYP, NCLIP, store SXY0..2 into the packet - so
         * the shadows shift with the registers. The MTC2 / LWC2 hook then
         * copies the source shadow into the pushed slot. */
        s_gte[12] = s_gte[13];
        s_gte[13] = s_gte[14];
        reg = 14;
    }
    pv_kill(&s_gte[reg]);
    pv_reset(&s_gte[reg], value);
    gte_sxy2_mirror(reg);                      /* SXY2 and SXYP: one register */
}

/* ------------------------------------------------------------------------- */
/* GPU consumer                                                               */
/* ------------------------------------------------------------------------- */

static inline int32_t field_rebase(int32_t v16, uint32_t half, int32_t parsed) {
    int32_t as16 = (int16_t)half;
    int32_t as11 = ((int32_t)(half << 21)) >> 21;
    if (parsed != as11 || parsed == as16) return v16;
    return (int32_t)((int64_t)v16 + ((int64_t)as11 - as16) * 65536);
}

extern "C" int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                                       int32_t int_x, int32_t int_y,
                                       int32_t *x16, int32_t *y16,
                                       uint16_t *sz) {
    s_stats.lookups++;

    int32_t px = 0, py = 0;
    uint16_t pz = 0;
    int have = 0;

    if (s_enabled && addr != 0xFFFFFFFFu) {
        PGXPValue *pv = pgxp_ptr(addr);
        if (pv && pv->gen == s_gen && (pv->flags & PGXP_F_VXY) != 0) {
            if (pv->value == packet_word &&
                (pv->flags & PGXP_F_VXY) == PGXP_F_VXY) {
                px = pv->x16;
                py = pv->y16;
                pz = (pv->flags & PGXP_F_VZ) ? pv->z : 0;
                have = PGXP_SRC_DATAFLOW;
            } else {
                s_stats.value_mismatch++;
            }
        }
    }

    if (!have && gte_geometry_correction_lookup(packet_word, &px, &py)) {
        pz = 0;                                /* fallback never carries depth */
        have = PGXP_SRC_FALLBACK;
    }

    if (have) {
        /* A shadow describes each half read as int16; GP0 decodes only its
         * 11-bit field (engines keep clip / outcode bits above it). When the
         * caller parsed the field, rebase the precise value onto it - the
         * fraction is unchanged. Any other integer is caught below. */
        px = field_rebase(px, packet_word & 0xFFFFu, int_x);
        py = field_rebase(py, packet_word >> 16, int_y);
        /* Truncation agreement: the GPU parsed 11-bit integers out of the
         * packet; a precise position whose integer part disagrees (a
         * wrapped/CPU-modified coordinate) must not be believed. */
        if ((px >> 16) != int_x || (py >> 16) != int_y) {
            s_stats.trunc_reject++;
            have = 0;
        } else if (s_tolerance >= 0.0f) {
            float dx = (float)(px - (int_x << 16)) * (1.0f / 65536.0f);
            float dy = (float)(py - (int_y << 16)) * (1.0f / 65536.0f);
            if (dx > s_tolerance || dy > s_tolerance) {
                s_stats.tolerance_reject++;
                have = 0;
            }
        }
    }

    if (!have) {
        s_stats.native++;
        *x16 = int_x << 16;
        *y16 = int_y << 16;
        *sz = 0;
        return PGXP_SRC_NATIVE;
    }

    if (have == PGXP_SRC_DATAFLOW) s_stats.dataflow_hit++;
    else                           s_stats.fallback_hit++;
    if (pz != 0) s_stats.w_valid++;
    *x16 = px;
    *y16 = py;
    *sz = pz;
    return have;
}

/* ------------------------------------------------------------------------- */
/* Legacy v14 SWC2 tracker — kept as the base-flavour feed                    */
/* ------------------------------------------------------------------------- */

/* Emitted at every swc2 since ABI v14 (all flavours, all backends, overlay
 * DLLs). In the pgxp flavour psx_pgxp_cop2 supersedes it for the same swc2 —
 * the double write is idempotent (same source shadow, same destination). */
extern "C" void pgxp_store_gte_reg(uint32_t addr, uint8_t reg) {
    if (!g_pgxp_active) return;
    PGXPValue *dst = pgxp_ptr_w(addr);
    if (!dst) return;
    const PGXPValue *src = &s_gte[reg & 31u];
    if (src->gen != s_gen) return;
    s_stats.swc2_stores++;
    *dst = *src;
}

extern "C" int pgxp_debug_shadow(int space, uint32_t key, int *live,
                                 uint32_t *value, uint32_t *flags,
                                 int32_t *x16, int32_t *y16, uint16_t *z) {
    const PGXPValue *pv = nullptr;
    if (space == 0)                pv = pgxp_ptr(key);
    else if (space == 1 && key < 34) pv = &s_gpr[key];
    else if (space == 2 && key < 32) pv = &s_gte[key];
    if (!pv) return 0;
    *live = pv->gen == s_gen;
    *value = pv->value;
    *flags = pv->flags;
    *x16 = pv->x16;
    *y16 = pv->y16;
    *z = pv->z;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Test accessors                                                             */
/* ------------------------------------------------------------------------- */

extern "C" void pgxp_test_seed_gte_sxy(uint32_t index, uint32_t packed,
                                       int32_t x16, int32_t y16, uint16_t z,
                                       int valid) {
    if (index >= 4) return;
    PGXPValue *pv = &s_gte[12 + index];
    pv->x16 = x16;
    pv->y16 = y16;
    pv->z = z;
    pv->value = packed;
    pv->flags = (uint16_t)(PGXP_F_VXY | (z != 0 ? PGXP_F_VZ : 0));
    pv->gen = valid ? s_gen : 0;
}

extern "C" void pgxp_test_get_gte_sxy(uint32_t index, uint32_t *packed,
                                      int32_t *x16, int32_t *y16, uint16_t *z,
                                      uint8_t *valid) {
    if (index >= 4) return;
    const PGXPValue *pv = &s_gte[12 + index];
    if (packed) *packed = pv->value;
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    if (z) *z = pv->z;
    if (valid) *valid = pv_live(pv) ? 1 : 0;
}

extern "C" uint32_t pgxp_test_generation(void) { return s_gen; }

extern "C" uint32_t pgxp_test_suppress_depth(void) { return s_suppress; }
extern "C" int pgxp_test_active(void) { return g_pgxp_active; }

extern "C" void pgxp_test_set_generation(uint32_t gen) { s_gen = gen; }

/* Address-keyed depth lookup for the shipped perspective-texturing path
 * (gpu.c prepare_texture_triangle). Same contract as the retired hashed
 * table: hit only when the tracked word matches the packet word exactly. */
/* Always-on ring of refused precise-word lookups: which packet word, what
 * the shadow at its address held, and why it was refused. Join it with
 * wtrace_dump (writer PC per address) to find the guest code path that drops
 * provenance. */
static PGXPWordMiss s_miss_ring[PGXP_MISS_RING_CAP];
static uint64_t     s_miss_seq = 0;

static void note_word_miss(uint32_t addr, uint32_t packed, const PGXPValue *pv,
                           uint8_t reason) {
    PGXPWordMiss *m = &s_miss_ring[s_miss_seq % PGXP_MISS_RING_CAP];
    m->seq = s_miss_seq++;
    m->addr = addr;
    m->packet = packed;
    m->shadow_value = pv ? pv->value : 0;
    m->shadow_flags = pv ? (uint8_t)pv->flags : 0;
    m->live = (pv && pv->gen == s_gen) ? 1 : 0;
    m->reason = reason;
}

extern "C" uint64_t pgxp_word_miss_ring(const PGXPWordMiss **ring,
                                        uint32_t *cap) {
    *ring = s_miss_ring;
    *cap = PGXP_MISS_RING_CAP;
    return s_miss_seq;
}

extern "C" int pgxp_load_precise_word(uint32_t addr, uint32_t packed,
                                      int32_t *x16, int32_t *y16, uint16_t *z) {
    if (!s_enabled) return 0;
    s_stats.word_lookups++;
    PGXPValue *pv = pgxp_ptr(addr);
    if (!pv || pv->gen != s_gen) {
        s_stats.word_untracked++;
        note_word_miss(addr, packed, pv, PGXP_MISS_UNTRACKED);
        return 0;
    }
    if (pv->value != packed) {
        s_stats.word_mismatch++;
        note_word_miss(addr, packed, pv, PGXP_MISS_MISMATCH);
        return 0;
    }
    if ((pv->flags & PGXP_F_VXY) != PGXP_F_VXY) {
        s_stats.word_partial++;
        note_word_miss(addr, packed, pv, PGXP_MISS_PARTIAL);
        return 0;
    }
    if (x16) *x16 = pv->x16;
    if (y16) *y16 = pv->y16;
    if (z) *z = (pv->flags & PGXP_F_VZ) ? pv->z : 0;
    if (!(pv->flags & PGXP_F_VZ) || pv->z == 0) {
        s_stats.word_no_z++;
        note_word_miss(addr, packed, pv, PGXP_MISS_NO_Z);
        return 0;
    }
    s_stats.word_hit++;
    return 1;
}
