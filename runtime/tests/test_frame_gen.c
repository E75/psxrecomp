/* Frame generation's renderer-independent half (src/frame_gen.c,
 * docs/FRAME_GENERATION.md): matching triangles of two frames, the
 * interpolation endpoints, the plan of how many in-between frames fit, and
 * the breaker, the generated-frame cost estimate and the guest pace. */
#include "frame_gen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); }
}

static FgPrim tri(uint32_t key, float x, float y, float s) {
    FgPrim p;
    memset(&p, 0, sizeof p);
    p.key = key;
    p.x[0] = x;     p.y[0] = y;
    p.x[1] = x + s; p.y[1] = y;
    p.x[2] = x;     p.y[2] = y + s;
    return p;
}

static void add(FgPrimList *l, FgPrim p) { p.rec = l->n; check(fg_prims_add(l, &p), "add"); }

static void test_identical_and_moved(void) {
    FgPrimList a = { 0 }, b = { 0 };
    for (int i = 0; i < 200; i++) add(&a, tri((uint32_t)(i % 7) + 1, (float)(i % 20) * 16.0f, (float)(i / 20) * 16.0f, 12.0f));
    for (int i = 0; i < 200; i++) add(&b, tri((uint32_t)(i % 7) + 1, (float)(i % 20) * 16.0f + 3.0f, (float)(i / 20) * 16.0f - 2.0f, 12.0f));
    FgMatchParams mp;
    fg_match_defaults(&mp);
    int32_t *m = (int32_t *)malloc(sizeof(int32_t) * b.n);
    FgMatchStats st;
    uint32_t got = fg_match(&a, &b, &mp, m, &st);
    check(got == 200 && st.matched == 200 && st.unmatched == 0, "a moved scene matches every triangle");
    int pairs_ok = 1;
    for (uint32_t i = 0; i < b.n; i++) if (m[i] != (int32_t)i) pairs_ok = 0;
    check(pairs_ok, "each triangle pairs with itself (draw order, nearest)");
    /* Same positions: zero moves. */
    got = fg_match(&a, &a, &mp, m, &st);
    check(got == 200 && st.moved == 0, "an unchanged scene matches without motion");
    free(m);
    fg_prims_free(&a); fg_prims_free(&b);
}

static void test_keys_limits_and_duplicates(void) {
    FgPrimList a = { 0 }, b = { 0 };
    FgMatchParams mp;
    fg_match_defaults(&mp);
    /* 0: different key never pairs. 1: moved past max_move. 2: deformed.
     * 3/4: two copies of one key; the newer ones swapped in draw order but
     * each still nearest its own. */
    add(&a, tri(10, 0, 0, 10));
    add(&a, tri(11, 0, 0, 10));
    add(&a, tri(12, 0, 0, 10));
    add(&a, tri(13, 100, 100, 10));
    add(&a, tri(13, 200, 100, 10));
    add(&b, tri(99, 0, 0, 10));
    add(&b, tri(11, mp.max_move + 10.0f, 0, 10));
    FgPrim d = tri(12, 0, 0, 10);
    d.x[1] += mp.max_deform + 8.0f;   /* one vertex moves alone */
    add(&b, d);
    add(&b, tri(13, 204, 101, 10));
    add(&b, tri(13, 103, 99, 10));
    int32_t m[5];
    FgMatchStats st;
    fg_match(&a, &b, &mp, m, &st);
    check(m[0] == -1, "a different key does not pair");
    check(m[1] == -1, "a move past max_move does not pair (placed, not moved)");
    check(m[2] == -1, "a triangle whose vertices moved apart does not pair");
    check(m[3] == 4 && m[4] == 3, "duplicate keys pair by distance");
    check(st.matched == 2 && st.unmatched == 3, "match stats");
    fg_prims_free(&a); fg_prims_free(&b);
}

static void test_each_older_pairs_once(void) {
    FgPrimList a = { 0 }, b = { 0 };
    add(&a, tri(5, 0, 0, 10));
    add(&b, tri(5, 0, 0, 10));
    add(&b, tri(5, 1, 0, 10));
    FgMatchParams mp;
    fg_match_defaults(&mp);
    int32_t m[2];
    fg_match(&a, &b, &mp, m, NULL);
    check(m[0] == 0 && m[1] == -1, "an older triangle pairs at most once");
    /* Empty older list: nothing pairs. */
    FgPrimList e = { 0 };
    fg_match(&e, &b, &mp, m, NULL);
    check(m[0] == -1 && m[1] == -1, "no history: nothing pairs");
    fg_prims_free(&a); fg_prims_free(&b);
}

static void test_lerp(void) {
    FgPrim a = tri(1, 10, 20, 8), b = tri(1, 30, 10, 8);
    float x[3], y[3];
    fg_lerp(&a, &b, 0.0, x, y);
    check(x[0] == a.x[0] && y[2] == a.y[2], "phase 0 is the older frame exactly");
    fg_lerp(&a, &b, 1.0, x, y);
    check(x[1] == b.x[1] && y[1] == b.y[1], "phase 1 is the newer frame exactly");
    fg_lerp(&a, &b, 0.25, x, y);
    check(fabsf(x[0] - 15.0f) < 1e-5f && fabsf(y[0] - 17.5f) < 1e-5f, "phase 0.25");
}

static void test_plan(void) {
    const double f30 = 2.0 / 59.94, f60 = 1.0 / 59.94;
    /* 30 Hz game frames on a 120 Hz panel: 4 slots, 3 in-between. */
    check(fg_plan(f30, 120.0, 0.010, 0.002, 0.85, 7) == 3, "30 Hz on 120 Hz: three in-between frames");
    /* 60 Hz game frames on 120 Hz: one. On 60 Hz: none. */
    check(fg_plan(f60, 120.0, 0.004, 0.002, 0.85, 7) == 1, "60 Hz on 120 Hz: one");
    check(fg_plan(f60, 60.0, 0.004, 0.002, 0.85, 7) == 0, "60 Hz on 60 Hz: none");
    check(fg_plan(f30, 60.0, 0.010, 0.002, 0.85, 7) == 1, "30 Hz on 60 Hz: one");
    /* Only what fits: 33.4 ms * 0.85 = 28.4; real 20 ms leaves 8.4 -> two of 4 ms. */
    check(fg_plan(f30, 120.0, 0.020, 0.004, 0.85, 7) == 2, "only what fits");
    check(fg_plan(f30, 120.0, 0.030, 0.001, 0.85, 7) == 0, "no surplus: none");
    /* Unknown generation cost: one, if half the budget is free. */
    check(fg_plan(f30, 120.0, 0.010, 0.0, 0.85, 7) == 1, "unmeasured: one to measure");
    check(fg_plan(f30, 120.0, 0.020, 0.0, 0.85, 7) == 0, "unmeasured without room: none");
    check(fg_plan(f30, 240.0, 0.001, 0.0001, 0.85, 2) == 2, "max_gens caps");
}

static void test_breaker(void) {
    FgBreaker b;
    fg_breaker_init(&b, 3.0, 24.0, 10.0);
    check(fg_breaker_open(&b, 0.0), "starts allowing generation");
    fg_breaker_trip(&b, 1.0, "late");
    check(!fg_breaker_open(&b, 3.9) && fg_breaker_open(&b, 4.0), "a trip holds 3 s");
    fg_breaker_trip(&b, 5.0, "late");   /* within 10 s of the last hold's end */
    check(!fg_breaker_open(&b, 10.9) && fg_breaker_open(&b, 11.0), "a repeat doubles the hold");
    fg_breaker_trip(&b, 6.0, "late");   /* already held: the hold stands */
    check(fg_breaker_open(&b, 11.0), "a trip while held does not extend it");
    fg_breaker_trip(&b, 40.0, "late");  /* long after: back to the base hold */
    check(!fg_breaker_open(&b, 42.9) && fg_breaker_open(&b, 43.0), "a later trip starts over");
    check(b.trips == 4 && b.reason && strcmp(b.reason, "late") == 0, "trips counted with a reason");
    for (int i = 0; i < 10; i++) fg_breaker_trip(&b, 43.0 + 30.0 * i + (i ? 0 : 0), "x");
    check(b.hold <= 24.0, "the hold is capped");
}

static void test_cost(void) {
    FgCost c;
    fg_cost_init(&c, 2.0, 16.0);
    const double fit = 0.020;
    check(fg_cost_estimate(&c, 0.0, fit) == 0.0, "unknown before any sample");
    /* Cold: the first frames after allocation are discarded. */
    fg_cost_cold(&c, 2);
    fg_cost_add(&c, 0.070, fit);
    fg_cost_add(&c, 0.040, fit);
    check(c.ema == 0.0 && c.discarded == 2, "cold samples discarded");
    fg_cost_add(&c, 0.006, fit);
    check(fabs(fg_cost_estimate(&c, 0.1, fit) - 0.006) < 1e-12, "first warm sample is the estimate");
    fg_cost_add(&c, 0.016, fit);
    check(fabs(c.ema - 0.008) < 1e-12, "warm samples blend");
    /* A spike that pushes the estimate past the fit blocks the plan... */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    check(fg_cost_estimate(&c, 10.0, fit) > fit, "a high estimate blocks");
    check(fg_cost_estimate(&c, 11.9, fit) > fit, "... until the probe interval");
    /* ...for at most probe_s: then one probe, whose sample replaces it. */
    check(fg_cost_estimate(&c, 12.0, fit) == 0.0 && c.probes == 1, "stale estimate re-probed");
    check(fg_cost_estimate(&c, 12.03, fit) > fit, "one probe at a time");
    fg_cost_add(&c, 0.005, fit);
    check(fabs(fg_cost_estimate(&c, 12.1, fit) - 0.005) < 1e-12, "the probe replaces the estimate");
    /* A probe that still does not fit backs off. */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    (void)fg_cost_estimate(&c, 20.0, fit);
    check(fg_cost_estimate(&c, 22.0, fit) == 0.0, "second probe");
    fg_cost_add(&c, 0.050, fit);
    check(c.probe_s == 4.0, "a probe that does not fit doubles the wait");
    check(fg_cost_estimate(&c, 25.9, fit) > fit && fg_cost_estimate(&c, 26.0, fit) == 0.0,
          "next probe after the doubled wait");
    fg_cost_add(&c, 0.004, fit);
    check(c.probe_s == 2.0, "a probe that fits resets the wait");
    /* A granted probe that was never drawn is given up, then re-granted. */
    for (int i = 0; i < 10; i++) fg_cost_add(&c, 0.060, fit);
    (void)fg_cost_estimate(&c, 40.0, fit);
    check(fg_cost_estimate(&c, 42.0, fit) == 0.0, "probe granted");
    check(fg_cost_estimate(&c, 44.0, fit) > fit, "an undrawn probe expires");
    check(fg_cost_estimate(&c, 46.0, fit) == 0.0, "and is granted again");
    /* Re-allocation makes the next samples cold again. */
    fg_cost_cold(&c, 2);
    fg_cost_add(&c, 0.5, fit); fg_cost_add(&c, 0.5, fit);
    check(c.cold == 0 && c.ema < 0.5, "cold after re-allocation");
}

static void test_pace(void) {
    FgPace p;
    memset(&p, 0, sizeof p);
    const double T = 1.0 / 59.94, slack = 2.0 * T;
    double t = 0.0;
    int late = 0;
    for (int i = 0; i < 600; i++) {   /* jitter: +-6 ms around the schedule */
        late += fg_pace_note(&p, t + ((i % 3) - 1) * 0.006, T, slack);
        t += T;
    }
    check(late == 0, "jitter is not late");
    late = 0;   /* one 40 ms frame made up by a burst */
    late += fg_pace_note(&p, t + 0.024, T, slack); t += T;
    late += fg_pace_note(&p, t + 0.004, T, slack); t += T;
    late += fg_pace_note(&p, t, T, slack); t += T;
    check(late == 0, "a long frame made up is not late");
    /* A real slip: 60 ms gap. */
    check(fg_pace_note(&p, t + 0.045, T, slack) == 1, "a slip beyond two intervals is late");
    t += 0.045 + T;
    check(fg_pace_note(&p, t, T, slack) == 0, "the schedule restarts after a slip");
    /* Slow but steady (a 50 Hz guest told 59.94): one trip, then resync each time it slips. */
    int trips = 0;
    for (int i = 0; i < 60; i++) { t += 0.020; trips += fg_pace_note(&p, t, T, slack); }
    check(trips > 0 && trips < 20, "a steadily slow guest is late now and then");
}

static void test_ceiling(void) {
    FgCeiling c;
    fg_ceiling_init(&c, 7, 2.0);
    check(fg_ceiling_get(&c, 0.0) == 7, "no ceiling before a trip");
    fg_ceiling_trip(&c, 3, 1.0);
    check(fg_ceiling_get(&c, 1.5) == 2, "an overload at three plans two");
    fg_ceiling_trip(&c, 2, 1.6);
    check(fg_ceiling_get(&c, 1.7) == 1, "another lowers it again");
    fg_ceiling_trip(&c, 1, 1.8);
    check(fg_ceiling_get(&c, 1.9) == 1, "never below one (the breaker stops generation)");
    check(fg_ceiling_get(&c, 3.7) == 1 && fg_ceiling_get(&c, 3.9) == 2, "recovers one per 2 s");
    check(fg_ceiling_get(&c, 7.9) == 4, "and keeps recovering");
    fg_ceiling_trip(&c, 0, 8.0);
    check(fg_ceiling_get(&c, 8.0) == 4, "a trip with nothing planned is not an overload");
    check(fg_ceiling_get(&c, 100.0) == 7, "up to the maximum");
}

int main(void) {
    test_ceiling();
    test_cost();
    test_pace();
    test_identical_and_moved();
    test_keys_limits_and_duplicates();
    test_each_older_pairs_once();
    test_lerp();
    test_plan();
    test_breaker();
    printf("frame_gen_test: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
