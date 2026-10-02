/* test_pgxp.cpp — PGXP value-propagation engine unit tests (docs/ENHANCEMENTS.md
 * G1.2/G1.3). White-box over runtime/src/pgxp.cpp with the gte.cpp fallback
 * cache stubbed, exercising exactly the properties the engine's safety rests
 * on: provenance roundtrips, validate-on-read, half-word semantics, the
 * repack arithmetic, the suppression bracket, and the GPU-side safeguards. */

#include "pgxp.h"
#include "pgxp_hooks.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- gte.cpp fallback-cache stub ----------------------------------------- */

static uint32_t g_fb_packed = 0;
static int32_t  g_fb_x16 = 0, g_fb_y16 = 0;
static int      g_fb_valid = 0;

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16) {
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

/* ---- MIPS encodings ------------------------------------------------------ */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh,
                      uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t enc_cop2(uint32_t sub, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}

#define LW(rs, rt)   enc_i(0x23, rs, rt, 0)
#define SW(rs, rt)   enc_i(0x2B, rs, rt, 0)
#define LH(rs, rt)   enc_i(0x21, rs, rt, 0)
#define LHU(rs, rt)  enc_i(0x25, rs, rt, 0)
#define SH(rs, rt)   enc_i(0x29, rs, rt, 0)
#define SB(rs, rt)   enc_i(0x28, rs, rt, 0)
#define LWC2(rt)     enc_i(0x32, 1, rt, 0)
#define SWC2(rt)     enc_i(0x3A, 1, rt, 0)
#define MFC2(rt, rd) enc_cop2(0x00, rt, rd)
#define MTC2(rt, rd) enc_cop2(0x04, rt, rd)
#define ADDIU(rs, rt, imm) enc_i(0x09, rs, rt, (uint16_t)(imm))
#define LUI(rt, imm) enc_i(0x0F, 0, rt, (uint16_t)(imm))
#define SLL(rt, rd, sh) enc_r(0, rt, rd, sh, 0x00)
#define SRA(rt, rd, sh) enc_r(0, rt, rd, sh, 0x03)
#define OR(rs, rt, rd)  enc_r(rs, rt, rd, 0, 0x25)
#define ADDU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x21)
#define SUBU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x23)
#define AND(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x24)
#define NOR(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x27)
#define SLT(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x2A)
#define ANDI(rs, rt, imm) enc_i(0x0C, rs, rt, (uint16_t)(imm))
#define ORI(rs, rt, imm) enc_i(0x0D, rs, rt, (uint16_t)(imm))

/* One projected vertex: x = 160.5, y = 80.25 -> packed integer word. */
static const uint32_t PACKED  = (80u << 16) | 160u;
static const int32_t  X16     = (160 << 16) | 0x8000;   /* 160.5  */
static const int32_t  Y16     = (80 << 16)  | 0x4000;   /* 80.25  */
static const uint16_t SZ3     = 100;

static const uint32_t ADDR_A  = 0x80100000u;   /* packet slot A (KSEG0)  */
static const uint32_t ADDR_B  = 0x00100040u;   /* packet slot B (KUSEG)  */

static void produce_at(uint32_t addr) {
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, SWC2(14), PACKED, addr);
}

static int lookup(uint32_t addr, uint32_t word, int32_t ix, int32_t iy,
                  int32_t *x, int32_t *y, uint16_t *z) {
    int32_t lx, ly; uint16_t lz;
    int r = pgxp_get_precise_vertex(addr, word, ix, iy, &lx, &ly, &lz);
    if (x) *x = lx;
    if (y) *y = ly;
    if (z) *z = lz;
    return r;
}

int main(void) {
    pgxp_set_enabled(1);
    pgxp_set_tolerance(-1.0f);
    pgxp_set_cpu_mode(0);

    /* --- SWC2 produce -> GPU consume (the perspective-texturing spine) --- */
    produce_at(ADDR_A);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* mirrors resolve to the same shadow word */
        CHECK(lookup(0xA0100000u, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- DMA/untracked overwrite: value validation rejects the shadow --- */
    {
        int32_t x, y; uint16_t z;
        uint32_t other = (81u << 16) | 161u;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (161 << 16) && y == (81 << 16) && z == 0);
    }

    /* --- LW/SW roundtrip: packet copied by the CPU keeps provenance --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
    }

    /* --- stale GPR: register changed between load and store --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, 0xDEADBEEFu);   /* r8 mutated */
    CHECK(lookup(ADDR_B, 0xDEADBEEFu, 0, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- MOVE idiom (memory mode, no cpu_mode needed) --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDU(8, 0, 10), PACKED, PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- MFC2 -> SW (register transfer path) --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(9, 14), PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- LH/SH: halves travel independently, depth does not survive --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B + 2u, PACKED >> 16);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);     /* X   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
        CHECK(z == 0);                       /* SH killed the vertex depth   */
    }

    /* --- SB destroys the touched half only --- */
    produce_at(ADDR_B);
    psx_pgxp_store(nullptr, SB(1, 8), ADDR_B, PACKED & 0xFFu);  /* same byte */
    {
        int32_t x, y; uint16_t z;
        /* low half invalidated -> not a full XY hit anymore */
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) != PGXP_SRC_DATAFLOW);
    }

    /* --- cpu-mode repack: lhu / sll 16 / or (the classic vertex build) --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_load(nullptr, LHU(1, 10), ADDR_A, PACKED & 0xFFFFu);    /* X   */
    psx_pgxp_alu(nullptr, OR(9, 10, 11), PACKED,
                 (PACKED >> 16) << 16, PACKED & 0xFFFFu);
    psx_pgxp_store(nullptr, SW(1, 11), ADDR_B, PACKED);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
    }

    /* --- cpu-mode addiu: fraction rides an integer offset (incl. -N) --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED + 4u);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
    }
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, (uint16_t)-4), PACKED - 4u, PACKED,
                 (uint32_t)(int32_t)-4);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED - 4u);
    {
        int32_t x;
        CHECK(lookup(ADDR_B, PACKED - 4u, 156, 80, &x, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 - (4 << 16));
    }
    pgxp_set_cpu_mode(0);

    /* --- cpu-mode OFF: the same repack must degrade to native, cleanly --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, (PACKED >> 16) << 16);
    CHECK(lookup(ADDR_B, (PACKED >> 16) << 16, 0, 80, nullptr, nullptr,
                 nullptr) == PGXP_SRC_NATIVE);

    /* --- clip flags packed above the GPU field (Spider-Man 0x8007F31C):
     *   mfc2 t2,SXY2; subu t9,t2,t4; nor t9,t9,t5; and t2,t2,t5; or t2,t2,t9
     *   ... and t2,t2,a3; or t2,t2,t9'; sw t2
     * The flag ops rewrite bits 14/15 and 30/31 of the word, never the 11-bit
     * fields GP0 decodes, so the vertex (and its depth) must survive - in
     * BOTH tiers, since bitwise carries are exact. --- */
    for (int mode = 0; mode < 2; mode++) {
        pgxp_set_cpu_mode(mode);
        const uint32_t T2 = 10, T4 = 12, T5 = 13, T9 = 25, A3 = 7;
        const uint32_t bounds = (240u << 16) | 128u;       /* x >= 128 -> flag */
        const uint32_t m14 = 0xBFFFBFFFu, m15 = 0x7FFF7FFFu;
        pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
        psx_pgxp_cop2(nullptr, MFC2(T2, 14), PACKED, 0);
        psx_pgxp_load(nullptr, LW(1, T4), 0x80180000u, bounds);  /* untracked */
        psx_pgxp_alu(nullptr, LUI(T5, 0xBFFF), 0xBFFF0000u, 0, 0);
        psx_pgxp_alu(nullptr, ORI(T5, T5, 0xBFFF), m14, 0xBFFF0000u, 0xBFFFu);
        uint32_t d = PACKED - bounds;
        psx_pgxp_alu(nullptr, SUBU(T2, T4, T9), d, PACKED, bounds);
        uint32_t f = ~(d | m14);
        psx_pgxp_alu(nullptr, NOR(T9, T5, T9), f, d, m14);
        uint32_t w = PACKED & m14;
        psx_pgxp_alu(nullptr, AND(T2, T5, T2), w, PACKED, m14);
        psx_pgxp_alu(nullptr, OR(T2, T9, T2), w | f, w, f);
        w |= f;
        CHECK(w == (PACKED | 0x4000u));                    /* x flagged      */
        psx_pgxp_alu(nullptr, AND(T2, A3, T2), w & m15, w, m15);
        w &= m15;
        psx_pgxp_load(nullptr, LW(1, T9), 0x80180004u, 0x80000000u);
        psx_pgxp_alu(nullptr, OR(T2, T9, T2), w | 0x80000000u, w, 0x80000000u);
        w |= 0x80000000u;
        psx_pgxp_store(nullptr, SW(1, T2), ADDR_B, w);
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, w, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        uint16_t wz = 0;
        CHECK(pgxp_load_precise_word(ADDR_B, w, nullptr, nullptr, &wz) == 1);
        CHECK(wz == SZ3);
    }
    pgxp_set_cpu_mode(0);

    /* --- quad from a projected-vertex table (Spider-Man 0x8007C5D4):
     *   lwc2 SXY0..2 <- table; NCLIP; lwc2 SXYP <- table; NCLIP;
     *   swc2 SXY0..2 -> packet
     * The SXYP write pushes the FIFO; the stored words are corners 1..3. --- */
    {
        const uint32_t TABLE = 0x80180100u, PKT = 0x80180200u;
        uint32_t words[4];
        for (uint32_t i = 0; i < 4; i++) {
            words[i] = ((80u + i) << 16) | (160u + i);
            pgxp_gte_push_sxy((int32_t)((160 + i) << 16) | 0x8000,
                              (int32_t)((80 + i) << 16) | 0x4000,
                              (uint16_t)(100 + i), words[i]);
            psx_pgxp_cop2(nullptr, SWC2(14), words[i], TABLE + i * 4u);
        }
        for (uint32_t i = 0; i < 3; i++) {          /* lwc2 SXY0..2        */
            pgxp_gte_reg_written((int)(12 + i), words[i]);
            psx_pgxp_cop2(nullptr, enc_i(0x32, 1, 12 + i, 0), words[i],
                          TABLE + i * 4u);
        }
        pgxp_gte_reg_written(15, words[3]);          /* lwc2 SXYP: push     */
        psx_pgxp_cop2(nullptr, enc_i(0x32, 1, 15, 0), words[3], TABLE + 12u);
        for (uint32_t i = 0; i < 3; i++)
            psx_pgxp_cop2(nullptr, SWC2(12 + i), words[i + 1], PKT + i * 4u);
        for (uint32_t i = 0; i < 3; i++) {
            int32_t x, y; uint16_t z;
            CHECK(lookup(PKT + i * 4u, words[i + 1], 161 + (int32_t)i,
                         81 + (int32_t)i, &x, &y, &z) == PGXP_SRC_DATAFLOW);
            CHECK(x == ((int32_t)((161 + i) << 16) | 0x8000));
            CHECK(z == 101 + i);
        }
    }

    /* --- render-pass checkpoint: a sandboxed pass rewrites a packet word
     * (raw-restored afterwards); the shadow must come back with it --- */
    {
        const uint32_t other = (90u << 16) | 170u;
        produce_at(ADDR_A);
        pgxp_checkpoint_begin();
        pgxp_gte_push_sxy((170 << 16) | 0x1000, (90 << 16) | 0x2000, 7, other);
        psx_pgxp_cop2(nullptr, SWC2(14), other, ADDR_A);
        CHECK(lookup(ADDR_A, other, 170, 90, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);                /* the pass sees its own word */
        pgxp_invalidate_all();                   /* undone by the rollback     */
        pgxp_suppress_begin();                   /* left open by an abort      */
        pgxp_checkpoint_rollback();
        CHECK(pgxp_test_suppress_depth() == 0);
        CHECK(pgxp_test_active());
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* a generation wrap inside the pass cannot be undone: fail closed */
        produce_at(ADDR_A);
        pgxp_checkpoint_begin();
        pgxp_test_set_generation(0xFFFFFFFFu);
        pgxp_invalidate_all();
        pgxp_checkpoint_rollback();
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
    }

    /* --- in-place ops read their source before the destination is reset --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 8, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED + 4u);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
        CHECK(z == SZ3);                       /* vertex + offset keeps depth */
    }
    pgxp_set_cpu_mode(0);

    /* --- a mask that changes the GPU field does not carry that half --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ANDI(8, 8, 0x0080), PACKED & 0x80u, PACKED, 0x80u);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED & 0x80u);
    CHECK(lookup(ADDR_B, PACKED & 0x80u, 128, 0, nullptr, nullptr, nullptr) !=
          PGXP_SRC_DATAFLOW);
    CHECK(pgxp_load_precise_word(ADDR_B, PACKED & 0x80u, nullptr, nullptr,
                                 nullptr) == 0);

    /* --- a saturated projection is not that half's value: never carried --- */
    {
        const uint32_t sat = (80u << 16) | 1023u;          /* x clamped     */
        pgxp_gte_push_sxy(2000 << 16, Y16, SZ3, sat);
        psx_pgxp_cop2(nullptr, MFC2(10, 14), sat, 0);
        psx_pgxp_load(nullptr, LW(1, 25), 0x80180008u, 0);  /* untracked 0   */
        psx_pgxp_alu(nullptr, OR(10, 25, 10), sat, sat, 0);
        psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, sat);
        CHECK(pgxp_load_precise_word(ADDR_B, sat, nullptr, nullptr, nullptr) == 0);
    }

    /* --- SLT-family results are not vertices: the destination resets --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(10, 14), PACKED, 0);
    psx_pgxp_alu(nullptr, SLT(0, 10, 10), 1u, 0, PACKED);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, 1u);
    CHECK(lookup(ADDR_B, 1u, 1, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- truncation agreement: integer part must match the native parse --- */
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 161, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- tolerance clamp --- */
    produce_at(ADDR_A);
    pgxp_set_tolerance(0.25f);                 /* fraction is 0.5 -> reject  */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_tolerance(0.75f);                 /* 0.5 <= 0.75 -> accept      */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_tolerance(-1.0f);

    /* --- fallback tier: no address -> position cache, never a depth --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_FALLBACK);
        CHECK(x == X16 && y == Y16 && z == 0);
    }
    g_fb_valid = 0;

    /* --- suppression bracket: nothing records inside it --- */
    pgxp_invalidate_all();
    pgxp_suppress_begin();
    produce_at(ADDR_A);
    pgxp_suppress_end();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- deferred invalidate inside the bracket --- */
    produce_at(ADDR_A);
    pgxp_suppress_begin();
    pgxp_invalidate_all();                     /* deferred                   */
    pgxp_suppress_end();                       /* applies here               */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- invalidate-all + generation wrap --- */
    produce_at(ADDR_A);
    pgxp_invalidate_all();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_test_set_generation(0xFFFFFFFFu);
    pgxp_invalidate_all();
    CHECK(pgxp_test_generation() == 1u);

    /* --- test accessors mirror the SXY FIFO shadows --- */
    pgxp_test_seed_gte_sxy(2, PACKED, X16, Y16, SZ3, 1);
    {
        uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(valid && packed == PACKED && x == X16 && y == Y16 && z == SZ3);
        pgxp_test_seed_gte_sxy(2, 0, 0, 0, 0, 0);
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(!valid);
    }

    /* --- stats sanity: dataflow hits were counted --- */
    {
        PGXPStats st;
        pgxp_get_stats(&st);
        CHECK(st.lookups > 0);
        CHECK(st.dataflow_hit > 0);
        CHECK(st.native > 0);
        CHECK(st.fallback_hit > 0);
        CHECK(st.value_mismatch > 0);
    }

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp: all checks passed\n");
    return 0;
}
