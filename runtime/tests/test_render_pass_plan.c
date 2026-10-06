/* Render-pass planning and presentation-selection math.
 * Build/run: ctest -R render_pass_plan_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "render_pass.h"
#include "render_pass_plan.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

/* One 30 Hz game frame (2 VBlanks at 59.94 Hz) in nanosecond ticks. */
static const double kFrame = 2.0 * 1e9 / 59.94;

static void test_counts_per_rate(void) {
    static const struct { double hz; uint32_t lo, hi; } rates[] = {
        {60.0, 1, 2}, {100.0, 3, 4}, {120.0, 3, 4}, {200.0, 6, 7},
        {240.0, 7, 8}, {300.0, 9, 10},
    };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        double period = 1e9 / rates[r].hz;
        /* Slide the output grid across one output period. */
        for (int k = 0; k < 16; k++) {
            RenderPassPlanInput in = {0};
            uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
            char msg[160];
            in.frame_start = 1e12;
            in.frame_length = kFrame;
            in.target_period = period;
            in.next_deadline = in.frame_start - 3.0 * period +
                               period * (double)k / 16.0;
            in.budget = -1.0;
            in.max = RENDER_PASS_MAX_PHASES;
            n = render_pass_plan_phases(&in, a, &wanted);
            snprintf(msg, sizeof msg, "%.0f Hz offset %d/16: %u passes",
                     rates[r].hz, k, (unsigned)n);
            CHECK(n >= rates[r].lo && n <= rates[r].hi, msg);
            CHECK(wanted == n, "unlimited budget sheds nothing");
            for (uint32_t i = 0; i < n; i++) {
                CHECK(a[i] > 0 && a[i] < 65536u, "phase inside (0, 1)");
                if (i) CHECK(a[i] > a[i - 1], "phases ascend");
                /* Each phase is an actual output deadline. */
                double d = in.frame_start + (double)a[i] / 65536.0 * kFrame;
                double m = fmod(d - in.next_deadline, period);
                if (m > period / 2) m -= period;
                snprintf(msg, sizeof msg,
                         "%.0f Hz phase %u lands on a deadline (off %.0f ns)",
                         rates[r].hz, (unsigned)i, m);
                CHECK(fabs(m) < kFrame / 65536.0 + 1.0, msg);
            }
        }
    }
}

static void test_shedding(void) {
    RenderPassPlanInput in = {0};
    uint32_t a[RENDER_PASS_MAX_PHASES], wanted = 0, n;
    in.frame_start = 1e12;
    in.frame_length = kFrame;
    in.target_period = 1e9 / 300.0;
    in.next_deadline = in.frame_start + 1e9 / 600.0;   /* half-period offset */
    in.max = RENDER_PASS_MAX_PHASES;
    in.pass_cost = 2e6;                                 /* 2 ms per pass */
    in.budget = 5e6;                                    /* 5 ms */
    n = render_pass_plan_phases(&in, a, &wanted);
    CHECK(wanted == 10, "300 Hz half-offset wants ten phases");
    CHECK(n == 2, "a 5 ms budget at 2 ms per pass affords two");
    CHECK(n == 2 && a[0] < 32768u && a[1] > 32768u,
          "a shed subset spreads across the frame");

    in.budget = 1e6;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 10,
          "less than one pass of budget renders none");

    /* No pass measured yet (first plans, or a new image size): passes run on
     * the emulation thread, so one measures the cost; a whole plan of
     * unknown cost could stall the guest for frames (46.9 ms each at 4K). */
    in.pass_cost = 0.0;
    in.budget = 26e6;
    n = render_pass_plan_phases(&in, a, &wanted);
    CHECK(n == 1 && wanted == 10, "unknown cost plans one pass");
    CHECK(n == 1 && a[0] > 16384u && a[0] < 49152u,
          "the one pass sits mid-frame, as a shed subset of one does");
    in.budget = 0.0;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 0,
          "no budget: not even the measuring pass");
    /* Leftover-time planning: the measuring pass only into at least
     * probe_min of leftover time (the caller stops it at the deadline). */
    in.probe_min = 4e6;                                 /* a quarter VBlank */
    in.budget = 26e6;
    n = render_pass_plan_phases(&in, a, NULL);
    CHECK(n == 1 && a[0] > 16384u && a[0] < 49152u,
          "leftover: unknown cost with probe_min left plans one pass");
    in.budget = 3.9e6;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 0,
          "leftover: unknown cost and less than probe_min left: none");
    in.budget = 0.0;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 0,
          "leftover: no leftover, not even a probe");
    in.probe_min = 0.0;
    in.pass_cost = 2e6;

    in.budget = -1.0;
    in.max = 4;
    CHECK(render_pass_plan_phases(&in, a, NULL) == 4,
          "capacity caps the plan");

    in.max = RENDER_PASS_MAX_PHASES;
    in.target_period = 0.0;
    CHECK(render_pass_plan_phases(&in, a, &wanted) == 0 && wanted == 0,
          "no live output schedule plans nothing");
}

static void test_select(void) {
    const uint32_t ph[] = {0u, 16384u, 32768u, 49152u};
    uint32_t lo = 99, hi = 99;
    float t = -1.0f;
    CHECK(render_pass_select(ph, 4, 0.0, &lo, &hi, &t) && lo == 0 && hi == 0,
          "phase 0 shows the game's image");
    CHECK(render_pass_select(ph, 4, 0.25, &lo, &hi, &t) && lo == 1 && hi == 1,
          "an exact pass phase shows that pass alone");
    CHECK(render_pass_select(ph, 4, 0.375, &lo, &hi, &t) && lo == 1 && hi == 2 &&
          fabsf(t - 0.5f) < 1e-4f,
          "between passes the neighbours blend");
    CHECK(render_pass_select(ph, 4, 0.9, &lo, &hi, &t) && lo == 3 && hi == 3,
          "past the last pass it holds (the next frame is not known yet)");
    CHECK(render_pass_select(ph, 1, 0.6, &lo, &hi, &t) && lo == 0 && hi == 0,
          "with only the game image it holds");
    CHECK(!render_pass_select(ph, 0, 0.5, &lo, &hi, &t), "no items, no pick");
    CHECK(render_pass_select(ph, 4, -0.1, &lo, &hi, &t) && lo == 0 && hi == 0,
          "before the frame starts show its first image");
}

/* A frame on screen longer than planned (a 30 Hz tick that takes three
 * VBlanks) keeps its newest image until the next flip; it must never go back
 * to the game's own image (phase 0), which is older. */
static void test_gen_select_late(void) {
    const uint32_t ph[] = {0u, 16384u, 32768u, 49152u};
    const double late[] = {1.0, 1.25, 1.2501, 1.3, 1.5, 2.0, 3.99,
                           RENDER_PASS_GEN_HOLD_MAX};
    uint32_t lo = 99, hi = 99;
    float t = -1.0f;
    CHECK(render_pass_gen_select(ph, 4, 0.375, &lo, &hi, &t) && lo == 1 &&
          hi == 2 && fabsf(t - 0.5f) < 1e-4f,
          "inside the frame it selects as render_pass_select");
    for (unsigned i = 0; i < sizeof late / sizeof late[0]; i++) {
        char msg[96];
        lo = hi = 99;
        snprintf(msg, sizeof msg, "late flip (p = %.4f) holds the newest image",
                 late[i]);
        CHECK(render_pass_gen_select(ph, 4, late[i], &lo, &hi, &t) &&
              lo == 3 && hi == 3 && t == 0.0f, msg);
    }
    CHECK(!render_pass_gen_select(ph, 4, RENDER_PASS_GEN_HOLD_MAX + 0.01,
                                  &lo, &hi, &t),
          "a game that stops flipping expires the frame after the hold");
    CHECK(!render_pass_gen_select(ph, 4, NAN, &lo, &hi, &t),
          "a phase that is not a number expires it");
    CHECK(!render_pass_gen_select(ph, 0, 0.5, &lo, &hi, &t), "no items, no pick");
}

/* Promotion: a generation for the next flip waits for its own rect; one for
 * a frame already on screen waits for the flip after it, to another rect. */
static void test_gen_flip_matches(void) {
    /* PsyQ VSync-then-PutDispEnv: built for the rect the flip shows. */
    CHECK(render_pass_gen_flip_matches(0, 0, 240, 1, 320, 240, 0, 240, 1, 320, 240),
          "pending: the flip to the generation's rect promotes it");
    CHECK(!render_pass_gen_flip_matches(0, 0, 240, 1, 320, 240, 0, 0, 1, 320, 240),
          "pending: a flip to the other buffer does not");
    /* Flip-when-drawn (V8:2): built for the rect already on screen. */
    CHECK(render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 0, 1, 320, 240),
          "shown: the next flip, to the other buffer, promotes it");
    CHECK(!render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 240, 1, 320, 240),
          "shown: a redraw of the rect on screen is not the next flip");
    /* Geometry the images were captured at must still be presented. */
    CHECK(!render_pass_gen_flip_matches(1, 0, 240, 1, 320, 240, 0, 0, 0, 320, 240),
          "a different presented source never promotes");
    CHECK(!render_pass_gen_flip_matches(0, 0, 0, 1, 320, 240, 0, 0, 1, 640, 480),
          "a different presented size never promotes");
}

static void test_budget_and_ema(void) {
    CHECK(fabs(render_pass_budget(0, 0, 100.0, 0.5) - 50.0) < 1e-9,
          "no history spends the share of the frame");
    CHECK(fabs(render_pass_budget(20.0, 10.0, 100.0, 0.8) - 24.0) < 1e-9,
          "idle plus pass time, scaled");
    CHECK(render_pass_budget(500.0, 0.0, 100.0, 0.8) == 100.0,
          "never more than a frame");
    CHECK(render_pass_budget(10.0, 10.0, 0.0, 0.8) == 0.0, "no frame, no budget");
    CHECK(render_pass_ema(0.0, 4.0) == 4.0, "first sample seeds the average");
    CHECK(fabs(render_pass_ema(4.0, 8.0) - 5.0) < 1e-9, "quarter-weight update");
    CHECK(render_pass_ema(4.0, -1.0) == 4.0, "bad samples are ignored");
    {
        /* First-use allocations (70 ms at 4K) must not set the average a
         * shed-for-time plan then never revisits; steady passes (10 ms) do. */
        RenderPassCost c;
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 0.0 && c.skips == 1 && c.kept == 0,
              "an allocating pass is not a sample");
        render_pass_cost_add(&c, 10.0, 0);
        render_pass_cost_add(&c, 10.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 0.0 && c.skips == 0,
              "two samples: still warming up (unknown: one pass per plan)");
        render_pass_cost_add(&c, 10.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 10.0, "three samples seed the average");
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 10.0,
              "a later allocating pass leaves it alone");
        for (unsigned i = 1; i < RENDER_PASS_ALLOC_SKIPS; i++)
            render_pass_cost_add(&c, 70.0, 1);
        CHECK(render_pass_cost_estimate(&c) == 10.0 &&
              c.skips == RENDER_PASS_ALLOC_SKIPS,
              "up to RENDER_PASS_ALLOC_SKIPS in a row");
        render_pass_cost_add(&c, 70.0, 1);
        CHECK(fabs(render_pass_cost_estimate(&c) - 25.0) < 1e-9 && c.skips == 0,
              "then an allocating pass counts, so the average cannot freeze");
        render_pass_cost_add(&c, -1.0, 0);
        render_pass_cost_add(&c, NAN, 0);
        CHECK(fabs(render_pass_cost_estimate(&c) - 25.0) < 1e-9,
              "bad samples are ignored");
    }
    {
        /* One slow first pass (a busy host: 24 ms against a steady 3 ms)
         * must not price passes out: the median of the warm-up wins. */
        RenderPassCost c;
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 24.0, 0);
        render_pass_cost_add(&c, 3.0, 0);
        render_pass_cost_add(&c, 3.5, 0);
        CHECK(render_pass_cost_estimate(&c) == 3.5,
              "warm-up median ignores one outlier");
        memset(&c, 0, sizeof c);
        render_pass_cost_add(&c, 47.0, 0);
        render_pass_cost_add(&c, 46.0, 0);
        render_pass_cost_add(&c, 48.0, 0);
        CHECK(render_pass_cost_estimate(&c) == 47.0,
              "a truly expensive size (4K) still prices itself out");
    }
    {
        /* The first passes of a race can all run in a transient (14.8 ms
         * against a steady 6.9 ms in a verify run). Priced out, no pass runs
         * to correct it: an estimate no pass was measured against for
         * RENDER_PASS_REWARM_MIN plans is measured again. */
        RenderPassCost c;
        unsigned i, rewarms = 0;
        memset(&c, 0, sizeof c);
        CHECK(!render_pass_cost_note_plan(&c), "unknown cost: nothing to re-measure");
        for (i = 0; i < RENDER_PASS_COST_WARMUP; i++) render_pass_cost_add(&c, 14.8, 0);
        CHECK(render_pass_cost_estimate(&c) == 14.8, "the transient sets the estimate");
        for (i = 1; i < RENDER_PASS_REWARM_MIN; i++)
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
        CHECK(rewarms == 0 && render_pass_cost_estimate(&c) == 14.8,
              "29 plans without a pass: still trusted");
        CHECK(render_pass_cost_note_plan(&c) && render_pass_cost_estimate(&c) == 0.0,
              "the 30th: unknown again, so plans ask for one pass");
        CHECK(!render_pass_cost_note_plan(&c), "no second restart while warming up");
        for (i = 0; i < RENDER_PASS_COST_WARMUP; i++) render_pass_cost_add(&c, 6.9, 0);
        CHECK(render_pass_cost_estimate(&c) == 6.9, "the steady cost replaces it");
        CHECK(c.rewarm_after == 0, "a stale estimate found: the wait stays at the minimum");
        /* Passes run on it: each measured pass restarts the count. */
        for (i = 0; i < 10u * RENDER_PASS_REWARM_MIN; i++) {
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
            render_pass_cost_add(&c, 6.9, 0);
        }
        CHECK(rewarms == 0, "an estimate passes run against is never restarted");
        /* Priced out again (a busier host): re-measured after the minimum. */
        for (i = 0; i < RENDER_PASS_REWARM_MIN; i++)
            rewarms += (unsigned)render_pass_cost_note_plan(&c);
        CHECK(rewarms == 1, "priced out again: re-measured after the minimum wait");
    }
    {
        /* A size that is truly too expensive (47 ms passes), or a machine at
         * its limit (10 ms re-measured as 9): each re-measure confirms the
         * estimate, so the waits double to RENDER_PASS_REWARM_MAX and the
         * one-pass warm-ups become rare. */
        static const double cases[][2] = {{47.0, 47.0}, {10.0, 9.0}};
        for (unsigned k = 0; k < 2; k++) {
            RenderPassCost c;
            unsigned i, plans = 0, waits[8] = {0}, w = 0, first = 1;
            memset(&c, 0, sizeof c);
            while (w < 8u && plans < 4000u) {
                if (render_pass_cost_estimate(&c) == 0.0) {
                    /* the warm-up's passes */
                    render_pass_cost_add(&c, first ? cases[k][0] : cases[k][1], 0);
                } else {
                    first = 0;
                    plans++;
                    if (render_pass_cost_note_plan(&c)) {
                        waits[w++] = plans;
                        plans = 0;
                    }
                }
            }
            CHECK(w == 8u && waits[0] == RENDER_PASS_REWARM_MIN &&
                  waits[1] == 2u * RENDER_PASS_REWARM_MIN &&
                  waits[2] == 4u * RENDER_PASS_REWARM_MIN,
                  "a confirmed estimate: the waits double");
            for (i = 5; i < 8; i++)
                CHECK(waits[i] == RENDER_PASS_REWARM_MAX, "and stop at the maximum");
        }
    }
}

static void test_store_policy(void) {
    CHECK(render_pass_mmio_class(0x1F801810u, 0x28000000u, 4) == -1, "GP0 reaches the GPU");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x04000002u, 4) == -1, "GP1 DMA mode allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x10000007u, 4) == -1, "GP1 info query allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x05000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 display start (a flip) is dropped");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x00000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 reset is dropped");
    CHECK(render_pass_mmio_class(0x1F8010A8u, 0x01000401u, 4) == -1, "GPU DMA CHCR allowed");
    CHECK(render_pass_mmio_class(0x1F8010E8u, 0x11000002u, 4) == -1, "OTC DMA allowed");
    CHECK(render_pass_mmio_class(0x1F8010F4u, 0, 4) == -1, "DICR allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F8010C8u, 0x01000201u, 4) == RENDER_PASS_DROP_DMA,
          "SPU DMA is dropped");
    CHECK(render_pass_mmio_class(0x1F801074u, 0, 4) == -1, "I_MASK allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F801D88u, 0x00FFu, 2) == RENDER_PASS_DROP_SPU,
          "SPU key-on is dropped (cannot be undone)");
    CHECK(render_pass_mmio_class(0x1F801C00u, 0, 2) == RENDER_PASS_DROP_SPU, "SPU voice regs dropped");
    CHECK(render_pass_mmio_class(0x1F801801u, 0x1Bu, 1) == RENDER_PASS_DROP_CD, "CD command dropped");
    CHECK(render_pass_mmio_class(0x1F801104u, 0, 4) == RENDER_PASS_DROP_TIMER, "timer mode dropped");
    CHECK(render_pass_mmio_class(0x1F801040u, 0, 1) == RENDER_PASS_DROP_OTHER, "SIO dropped");
    CHECK(render_pass_mmio_class(0x1F801820u, 0, 4) == RENDER_PASS_DROP_OTHER, "MDEC dropped");
}

static void test_stereo_pair_fresh(void) {
    const uint32_t vb = 564480u;
    const uint64_t c = 100000000ull;
    CHECK(render_pass_stereo_pair_fresh(c, c, vb), "pair fresh on its own cycle");
    CHECK(render_pass_stereo_pair_fresh(c, c + 8ull * vb, vb), "pair fresh across the slowest cadence");
    CHECK(render_pass_stereo_pair_fresh(c, c + 24ull * vb, vb), "pair fresh at the age limit");
    CHECK(!render_pass_stereo_pair_fresh(c, c + 24ull * vb + 1, vb),
          "pair stale once the plugin stops submitting");
    CHECK(!render_pass_stereo_pair_fresh(c, c - 1, vb), "clock behind pair (state load) is stale");
}

static void test_leftover_and_reserve(void) {
    /* The frame starts at 100 ms; 10 ms of work is still due before it. */
    CHECK(fabs(render_pass_leftover(80.0, 100.0, 10.0, 1.0) - 9.0) < 1e-9,
          "leftover = frame start - reserve - margin - now");
    CHECK(render_pass_leftover(95.0, 100.0, 10.0, 1.0) < 0.0,
          "past the point where the game needs the thread: none");
    CHECK(render_pass_leftover(80.0, 0.0, 10.0, 1.0) < 0.0,
          "no frame start known: none");
    CHECK(fabs(render_pass_leftover(80.0, 100.0, -5.0, NAN) - 20.0) < 1e-9,
          "bad reserve and margin count as zero");
    /* The reserve errs late: a larger sample at once, a smaller one slowly. */
    CHECK(render_pass_reserve_update(0.0, 6.0) == 6.0, "first sample sets it");
    CHECK(render_pass_reserve_update(6.0, 9.0) == 9.0, "a larger sample at once");
    {
        double r = 9.0;
        r = render_pass_reserve_update(r, 3.0);
        CHECK(fabs(r - (9.0 - 6.0 * RENDER_PASS_RESERVE_DECAY)) < 1e-9,
              "a smaller sample moves it down by the decay");
        for (int i = 0; i < 400; i++) r = render_pass_reserve_update(r, 3.0);
        CHECK(r > 3.0 && r < 3.01, "and it settles on a steady sample");
    }
    CHECK(render_pass_reserve_update(5.0, NAN) == 5.0 &&
          render_pass_reserve_update(5.0, -1.0) == 5.0, "bad samples are ignored");
}

static void test_resolution_admission(void) {
    CHECK(render_pass_admission_pct(1, 0) == 65 &&
          render_pass_admission_pct(3, 0) == 65,
          "native through 3x uses the wider admission");
    CHECK(render_pass_admission_pct(4, 0) == 50 &&
          render_pass_admission_pct(9, 0) == 50,
          "higher scales keep the conservative admission");
    CHECK(render_pass_admission_pct(9, 80) == 80,
          "an explicit profiling budget remains available");
    CHECK(render_pass_probe_min(4.0, 3.0, 16.0, 1) == 7.0 &&
          render_pass_probe_min(4.0, 3.0, 16.0, 9) == 14.0,
          "a high-resolution probe needs twice the normal work");
    CHECK(render_pass_probe_min(1.0, 1.0, 16.0, 1) == 4.0,
          "a low-resolution probe keeps the quarter-VBlank floor");
}

static void test_leftover_cost(void) {
    {
        /* First-use allocations are not samples; steady passes are, and the
         * first one sets the average. */
        RenderPassLeftoverCost c;
        memset(&c, 0, sizeof c);
        render_pass_leftover_cost_add(&c, 70.0, 1);
        CHECK(render_pass_leftover_cost_estimate(&c) == 0.0 && c.skips == 1 &&
              c.kept == 0, "an allocating pass is not a sample");
        render_pass_leftover_cost_add(&c, 10.0, 0);
        CHECK(c.kept == 1 && c.ema == 10.0 && c.skips == 0,
              "the first completed pass sets the average");
        CHECK(fabs(render_pass_leftover_cost_estimate(&c) - 15.0) < 1e-9,
              "planned with twice the deviation as headroom (2 x 2.5)");
        for (int i = 0; i < 40; i++) render_pass_leftover_cost_add(&c, 10.0, 0);
        CHECK(fabs(render_pass_leftover_cost_estimate(&c) - 10.0) < 0.01,
              "a steady cost loses its headroom");
        render_pass_leftover_cost_add(&c, 70.0, 1);
        CHECK(fabs(render_pass_leftover_cost_estimate(&c) - 10.0) < 0.01,
              "a later allocating pass leaves it alone");
        for (unsigned i = 1; i < RENDER_PASS_ALLOC_SKIPS; i++)
            render_pass_leftover_cost_add(&c, 70.0, 1);
        CHECK(c.skips == RENDER_PASS_ALLOC_SKIPS, "up to RENDER_PASS_ALLOC_SKIPS in a row");
        render_pass_leftover_cost_add(&c, 70.0, 1);
        CHECK(fabs(c.ema - 25.0) < 0.01 && c.skips == 0,
              "then an allocating pass counts, so the average cannot freeze");
        render_pass_leftover_cost_add(&c, -1.0, 0);
        render_pass_leftover_cost_add(&c, NAN, 0);
        CHECK(fabs(c.ema - 25.0) < 0.01, "bad samples are ignored");
    }
    {
        /* A pass stopped at its deadline cost more than it ran: a lower
         * bound, until a pass completes. */
        RenderPassLeftoverCost c;
        memset(&c, 0, sizeof c);
        render_pass_leftover_cost_cut(&c, 9.0, 0);
        CHECK(render_pass_leftover_cost_estimate(&c) == 9.0 && c.kept == 0,
              "a cut with nothing measured: the bound is the estimate");
        render_pass_leftover_cost_cut(&c, 7.0, 0);
        CHECK(render_pass_leftover_cost_estimate(&c) == 9.0,
              "a shorter cut lowers nothing");
        render_pass_leftover_cost_add(&c, 4.0, 0);
        CHECK(c.bound == 0.0 && c.ema == 4.0, "a completed pass clears the bound");
        render_pass_leftover_cost_cut(&c, 12.0, 0);
        CHECK(render_pass_leftover_cost_estimate(&c) == 12.0,
              "a cut above the average raises the estimate to it");
        render_pass_leftover_cost_cut(&c, NAN, 0);
        CHECK(render_pass_leftover_cost_estimate(&c) == 12.0, "bad cuts are ignored");
        render_pass_leftover_cost_cut(&c, 50.0, 1);
        CHECK(render_pass_leftover_cost_estimate(&c) == 12.0 && c.skips == 1,
              "a cut pass that allocated (one-time setup) bounds nothing");
        /* A bound from one stopped pass can be a busy moment: it is probed
         * like any estimate that prices plans out, and a probe far below it
         * replaces it. */
        {
            unsigned due = 0;
            for (unsigned i = 0; i < RENDER_PASS_PROBE_MIN; i++)
                due += (unsigned)render_pass_leftover_cost_probe_due(&c);
            CHECK(due == 1 && c.probing, "a bound is probed after the minimum wait");
            render_pass_leftover_cost_add(&c, 3.0, 0);
            CHECK(c.bound == 0.0 && c.ema == 3.0, "a fast probe replaces the bound");
        }
    }
    {
        /* The first passes of a race can run in a transient (14.8 ms against
         * a steady 6.9 ms). Priced out, no pass runs to correct it: after
         * RENDER_PASS_PROBE_MIN plans that had leftover time and planned
         * none, one probe runs (stopped at the deadline like any pass). */
        RenderPassLeftoverCost c;
        unsigned i, due = 0;
        memset(&c, 0, sizeof c);
        CHECK(!render_pass_leftover_cost_probe_due(&c),
              "unknown cost: plans probe anyway");
        render_pass_leftover_cost_add(&c, 14.8, 0);
        for (i = 1; i < RENDER_PASS_PROBE_MIN; i++)
            due += (unsigned)render_pass_leftover_cost_probe_due(&c);
        CHECK(due == 0, "29 priced-out plans: no probe yet");
        CHECK(render_pass_leftover_cost_probe_due(&c) && c.probing, "the 30th probes");
        CHECK(!render_pass_leftover_cost_probe_due(&c),
              "no second probe while one is open");
        render_pass_leftover_cost_add(&c, 6.9, 0);
        CHECK(c.ema == 6.9 && c.kept == 1 && !c.probing && c.probe_after == 0,
              "the probe found it stale: replaced, the wait stays at the minimum");
        /* Passes run on it: each measured pass restarts the count. */
        for (i = 0; i < 10u * RENDER_PASS_PROBE_MIN; i++) {
            render_pass_leftover_cost_add(&c, 6.9, 0);
            due += (unsigned)render_pass_leftover_cost_probe_due(&c);
        }
        CHECK(due == 0, "an estimate passes run against is never probed");
    }
    {
        /* A size that is truly too expensive, or a machine at its limit:
         * each probe is stopped at the deadline (or confirms the estimate),
         * so the waits double to RENDER_PASS_PROBE_MAX and probes become
         * rare. */
        for (unsigned k = 0; k < 2; k++) {
            RenderPassLeftoverCost c;
            unsigned plans = 0, waits[8] = {0}, w = 0;
            memset(&c, 0, sizeof c);
            render_pass_leftover_cost_add(&c, 47.0, 0);
            while (w < 8u && plans < 20000u) {
                plans++;
                if (render_pass_leftover_cost_probe_due(&c)) {
                    waits[w++] = plans;
                    plans = 0;
                    if (k == 0) render_pass_leftover_cost_cut(&c, 9.0, 0);
                    else render_pass_leftover_cost_add(&c, 46.0, 0);
                }
            }
            CHECK(w == 8u && waits[0] == RENDER_PASS_PROBE_MIN &&
                  waits[1] == 2u * RENDER_PASS_PROBE_MIN &&
                  waits[2] == 4u * RENDER_PASS_PROBE_MIN,
                  k ? "a confirming probe: the waits double"
                    : "a cut probe: the waits double");
            for (unsigned i = 5; i < 8; i++)
                CHECK(waits[i] == RENDER_PASS_PROBE_MAX, "and stop at the maximum");
            CHECK(render_pass_leftover_cost_estimate(&c) >= 46.0,
                  "a cut probe never lowers the estimate");
        }
    }
}

/* The admission share and probe minimum keyed on measured GPU pressure match
 * the scale-keyed ones (above 3x = the GPU is the limit), and the pressure
 * starts heavy, relaxes after RENDER_PASS_GPU_CALM_PLANS calm plans and is
 * heavy again at the next event. */
static void test_measured_admission(void) {
    RenderPassGpuPressure p;
    unsigned i;
    for (int sc = 1; sc <= 16; sc++) {
        CHECK(render_pass_admission_pct(sc, 0) ==
              render_pass_admission_pct_for(sc > 3, 0),
              "admission by scale is admission by pressure (scale > 3)");
        CHECK(render_pass_probe_min(4.0, 3.0, 16.0, sc) ==
              render_pass_probe_min_for(4.0, 3.0, 16.0, sc > 3),
              "probe minimum by scale is by pressure (scale > 3)");
    }
    CHECK(render_pass_admission_pct_for(1, 0) == 50 &&
          render_pass_admission_pct_for(0, 0) == 65 &&
          render_pass_admission_pct_for(1, 80) == 80,
          "admission: 50 when the GPU is the limit, 65 otherwise, override wins");
    memset(&p, 0, sizeof p);
    CHECK(render_pass_gpu_pressure_heavy(&p), "pressure: heavy before any plan");
    for (i = 0; i + 1 < RENDER_PASS_GPU_CALM_PLANS; i++)
        render_pass_gpu_pressure_note(&p, 0);
    CHECK(render_pass_gpu_pressure_heavy(&p), "pressure: still heavy one plan short");
    render_pass_gpu_pressure_note(&p, 0);
    CHECK(!render_pass_gpu_pressure_heavy(&p), "pressure: calm after the calm plans");
    for (i = 0; i < 1000; i++) render_pass_gpu_pressure_note(&p, 0);
    CHECK(!render_pass_gpu_pressure_heavy(&p), "pressure: stays calm");
    render_pass_gpu_pressure_note(&p, 1);
    CHECK(render_pass_gpu_pressure_heavy(&p), "pressure: an event makes it heavy");
    CHECK(render_pass_gpu_pressure_heavy(NULL), "pressure: none known is heavy");
}

/* Costs are kept per image size: switching back restores them; a size
 * never learnt starts empty; a probe in flight is not carried; nothing is
 * stored for a size that learnt nothing; the least recently used size is
 * replaced when the cache is full. */
static void test_cost_cache(void) {
    RenderPassCostCache cache;
    RenderPassCost cost;
    RenderPassLeftoverCost lcost;
    int w = 0, h = 0, i;
    memset(&cache, 0, sizeof cache);
    memset(&cost, 0, sizeof cost);
    memset(&lcost, 0, sizeof lcost);
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 960, 720) == 0 &&
          w == 960 && h == 720, "cache: first size, nothing known");
    CHECK(cache.n == 0, "cache: nothing stored for no size");
    render_pass_leftover_cost_add(&lcost, 4.0, 0);
    render_pass_cost_add(&cost, 3.0, 0);
    lcost.probing = 1;
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 1280, 720) == 0,
          "cache: a new size is unknown");
    CHECK(lcost.kept == 0 && cost.kept == 0 && lcost.ema == 0.0,
          "cache: a new size starts empty");
    CHECK(cache.n == 1, "cache: the learnt size was stored");
    /* 1280x720 learns nothing; back to 960x720. */
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 960, 720) == 1,
          "cache: the old size is found");
    CHECK(cache.n == 1, "cache: an unlearnt size is not stored");
    CHECK(lcost.kept == 1 && lcost.ema == 4.0 && cost.kept == 1 && !lcost.probing,
          "cache: costs restored, probe dropped");
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 960, 720) == 1 &&
          lcost.ema == 4.0, "cache: the same size is a no-op");
    /* Fill: sizes 100..108 wide, each learnt. 960x720 is the oldest. */
    for (i = 0; i <= (int)RENDER_PASS_COST_CACHE; i++) {
        render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 100 + i, 100);
        render_pass_leftover_cost_add(&lcost, (double)(10 + i), 0);
    }
    CHECK(cache.n == RENDER_PASS_COST_CACHE, "cache: bounded");
    render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 50, 50);
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 960, 720) == 0,
          "cache: the least recently used size was replaced");
    CHECK(render_pass_cost_cache_switch(&cache, &w, &h, &cost, &lcost, 100 + (int)RENDER_PASS_COST_CACHE, 100) == 1 &&
          lcost.ema == (double)(10 + RENDER_PASS_COST_CACHE),
          "cache: a recent size is still there");
}

int main(void) {
    test_leftover_and_reserve();
    test_resolution_admission();
    test_measured_admission();
    test_cost_cache();
    test_leftover_cost();
    test_stereo_pair_fresh();
    test_store_policy();
    test_counts_per_rate();
    test_shedding();
    test_select();
    test_gen_select_late();
    test_gen_flip_matches();
    test_budget_and_ema();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
