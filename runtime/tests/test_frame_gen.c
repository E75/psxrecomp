/* Frame generation's renderer-independent half (src/frame_gen.c,
 * docs/FRAME_GENERATION.md): matching triangles of two frames, the
 * interpolation endpoints, the plan of how many in-between frames fit, and
 * the breaker. */
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

int main(void) {
    test_identical_and_moved();
    test_keys_limits_and_duplicates();
    test_each_older_pairs_once();
    test_lerp();
    test_plan();
    test_breaker();
    printf("frame_gen_test: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
