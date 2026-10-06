/* The render-thread dynamic-resolution controller (dynamic_resolution.c,
 * dynrt_*) against a synthetic two-stage pipeline: the guest takes E per
 * frame, the render thread R(S) = a + b * S^2 (S the level), at most two
 * frames in flight. A frame's interval is max(period, E, R); when R is the
 * slowest stage the guest blocks on the queue for the difference
 * (backpressure). Costs reach the controller `lag` frames late, at the level
 * the frame was rendered at, like the GPU timestamp readback.
 * Checks: overrun -> down one level at a time, headroom -> up only after the
 * window, hysteresis (no oscillation), guest-bound -> no step, a down step
 * that does not lower the cost -> undone and backed off, queue full ->
 * fast down, floor and ceiling, holds.
 * Build/run: ctest -R dynamic_resolution_rt_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dynamic_resolution.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
    failures++; } } while (0)

static const double kPeriod = 1.0 / 59.94;
#define LAG 4

typedef struct Scene {
    double a, b;              /* render cost a + b*S^2 (seconds) */
    double emu;               /* guest time per frame */
    double flat;              /* > 0: render cost independent of S (seconds) */
    double under;             /* report cost * (1 - under): a meter that underreads */
    double heavy_from, heavy_to, heavy_a;   /* optional time-limited extra cost */
} Scene;

static double render_cost(const Scene *sc, int s, double t) {
    double r = sc->flat > 0.0 ? sc->flat : sc->a + sc->b * (double)s * (double)s;
    if (sc->heavy_to > sc->heavy_from && t >= sc->heavy_from && t < sc->heavy_to)
        r += sc->heavy_a;
    return r;
}

typedef struct Run {
    double t;
    int    level, steps, downs, ups;
    int    min_level, max_level;
    double first_down_t, first_up_t;
    double time_at[DYNRES_MAX_LEVEL + 1];
    double lagq[LAG];
    int    lagn;
    double frames;            /* guest frames */
} Run;

static void run_init(Run *r, int level) {
    memset(r, 0, sizeof *r);
    r->level = level;
    r->min_level = r->max_level = level;
    r->first_down_t = r->first_up_t = -1.0;
}

static void drive(DynrtController *c, Run *r, const Scene *sc, double seconds,
                  double held_until) {
    const double end = r->t + seconds;
    while (r->t < end) {
        const double R = render_cost(sc, r->level, r->t);
        double base = kPeriod > sc->emu ? kPeriod : sc->emu;
        double interval = R > base ? R : base;
        double bp = R > base ? R - base : 0.0;
        r->t += interval;
        r->frames += 1.0;
        r->time_at[r->level] += interval;
        /* the cost of the frame rendered LAG frames ago */
        int frames = 0;
        double cost = 0.0;
        if (r->lagn == LAG) {
            frames = 1;
            cost = r->lagq[0] * (1.0 - sc->under);
            memmove(r->lagq, r->lagq + 1, sizeof(double) * (LAG - 1));
            r->lagn--;
        }
        r->lagq[r->lagn++] = R;
        DynrtSample s = { kPeriod, interval, frames, cost, bp, r->t < held_until };
        int l = dynrt_sample(c, r->t, &s);
        if (l != r->level) {
            r->steps++;
            if (l < r->level) { r->downs++; if (r->first_down_t < 0) r->first_down_t = r->t; }
            else              { r->ups++;   if (r->first_up_t < 0) r->first_up_t = r->t; }
            r->level = l;
            if (l < r->min_level) r->min_level = l;
            if (l > r->max_level) r->max_level = l;
        }
    }
}

static void ctl(DynrtController *c, int floor_l, int ceil_l, int level) {
    DynrtParams p;
    dynrt_default_params(&p);
    dynrt_init(c, &p, floor_l, ceil_l, level);
}

/* Highest level whose cost fits the 15 % margin. */
static int fit_level(const Scene *sc, int floor_l, int ceil_l) {
    int best = floor_l;
    for (int s = floor_l; s <= ceil_l; s++)
        if (render_cost(sc, s, 0.0) <= kPeriod * 0.85) best = s;
    return best;
}

/* 1. Overrun at the ceiling: down one level at a time to the highest level
 *    that fits, fast, then no oscillation. */
static void test_overrun_steps_down_and_settles(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.003, .b = 0.00030 };   /* 9x: 27.3 ms, 6x: 13.8 ms, 7x: 17.7 ms */
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 20.0, 0.0);
    int want = fit_level(&sc, 3, 9);
    CHECK(r.level == want, "overrun: settled at %d, want %d", r.level, want);
    CHECK(r.first_down_t >= 0.0 && r.first_down_t < 0.6,
          "overrun: first down at %.2f s, want < 0.6 s", r.first_down_t);
    CHECK(r.ups == 0 && r.min_level == want, "overrun: never below %d (min %d, ups %d)",
          want, r.min_level, r.ups);
    /* every step a single level */
    CHECK(r.downs == 9 - want, "overrun: %d downs for %d levels", r.downs, 9 - want);
    int steps = r.steps;
    drive(&c, &r, &sc, 120.0, 0.0);
    CHECK(r.steps == steps, "overrun: %d steps after settling (oscillation)", r.steps - steps);
    CHECK(r.time_at[want] > 115.0, "overrun: %.1f s at the settled level", r.time_at[want]);
}

/* 2. Headroom: up one level, never before up_after_s, up to the ceiling. */
static void test_headroom_steps_up_after_window(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.001, .b = 0.00005 };   /* 9x: 5 ms */
    ctl(&c, 3, 9, 4);
    run_init(&r, 4);
    drive(&c, &r, &sc, 2.9, 0.0);
    CHECK(r.ups == 0, "headroom: up within %.1f s (%d ups)", 2.9, r.ups);
    drive(&c, &r, &sc, 60.0, 0.0);
    CHECK(r.first_up_t >= 3.0, "headroom: first up at %.2f s", r.first_up_t);
    CHECK(r.level == 9 && r.downs == 0, "headroom: level %d, downs %d", r.level, r.downs);
    CHECK(r.ups == 5, "headroom: %d ups for 5 levels (one at a time)", r.ups);
}

/* 3. Hysteresis band: a level at 0.80 of the interval stays (under the 0.85
 *    budget) and the next one (predicted over up_load) is never tried. */
static void test_hysteresis(void) {
    DynrtController c; Run r;
    /* 5x: 0.80 * period */
    double target = 0.80 * kPeriod;
    Scene sc = { .a = 0.0, .b = target / 25.0 };
    ctl(&c, 3, 9, 5);
    run_init(&r, 5);
    drive(&c, &r, &sc, 120.0, 0.0);
    CHECK(r.steps == 0, "hysteresis: %d steps at a level inside the band", r.steps);
}

/* 4. Guest-bound: the guest takes 25 ms per frame, the render thread 15.5 ms
 *    (0.93 of the nominal interval, over the budget). The guest never waits
 *    on the queue and the render thread has slack in the 25 ms it is given:
 *    resolution cannot help, so no down step. */
static void test_guest_bound_no_step(void) {
    DynrtController c; Run r;
    Scene sc = { .flat = 0.0155, .emu = 0.025 };
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 60.0, 0.0);
    CHECK(r.downs == 0, "guest-bound: %d down steps", r.downs);
    CHECK(c.guest_bound_windows > 100, "guest-bound: %llu guest-bound windows",
          c.guest_bound_windows);
    /* The render cost scales with S here, the guest is still the limit. */
    DynrtController c2; Run r2;
    Scene sc2 = { .a = 0.004, .b = 0.00014, .emu = 0.026 };   /* 9x: 15.3 ms */
    ctl(&c2, 3, 9, 9);
    run_init(&r2, 9);
    drive(&c2, &r2, &sc2, 60.0, 0.0);
    CHECK(r2.downs == 0, "guest-bound (scaling): %d down steps", r2.downs);
}

/* 5. A down step that does not lower the cost (render-thread submission
 *    bound, not pixels): undone, and down steps back off (doubling). */
static void test_undo_and_backoff(void) {
    DynrtController c; Run r;
    Scene sc = { .flat = 0.0175 };   /* 1.05 of the period at every level */
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 3.0, 0.0);
    CHECK(c.undos == 1 && r.level == 9, "undo: undos %llu, level %d after 3 s", c.undos, r.level);
    drive(&c, &r, &sc, 117.0, 0.0);
    /* two steps per try, blocks of 20, 40, 80 s: at most 3 tries in 120 s */
    CHECK(r.downs <= 6, "back-off: %d down steps in 120 s", r.downs);
    CHECK(r.time_at[9] > 112.0, "back-off: %.1f s at the ceiling", r.time_at[9]);
    CHECK(r.min_level >= 7, "back-off: one level at a time (min %d)", r.min_level);
}

/* 6. The queue is full but the meter underreads (load 0.75 reported): the
 *    backpressure alone steps down, one window each. */
static void test_queue_full_steps_down(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.003, .b = 0.00030, .under = 0.45 };
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 0.4, 0.0);
    CHECK(r.downs >= 1, "queue full: no down step within 0.4 s");
    drive(&c, &r, &sc, 20.0, 0.0);
    CHECK(r.level <= fit_level(&sc, 3, 9) + 1 && r.level >= 3,
          "queue full: level %d", r.level);
    /* No backpressure left at the level it settled at. */
    CHECK(render_cost(&sc, r.level, 0.0) <= kPeriod + 1e-9,
          "queue full: settled at %d still behind", r.level);
}

/* 7. Floor and ceiling. */
static void test_floor_and_ceiling(void) {
    DynrtController c; Run r;
    Scene heavy = { .a = 0.010, .b = 0.0010 };   /* over budget at every level */
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &heavy, 60.0, 0.0);
    CHECK(r.min_level == 3 && r.level == 3, "floor: min %d level %d", r.min_level, r.level);
    DynrtController c2; Run r2;
    Scene light = { .a = 0.001 };
    ctl(&c2, 3, 9, 9);
    run_init(&r2, 9);
    drive(&c2, &r2, &light, 60.0, 0.0);
    CHECK(r2.max_level == 9 && r2.steps == 0, "ceiling: max %d steps %d", r2.max_level, r2.steps);
}

/* 8. A heavy stretch: down, then back up once it is over (one level at a
 *    time, after the window), and no relapse churn. Its first window mixes
 *    light and heavy frames, so the first step looks like it removed
 *    nothing: one strike, not an undo. */
static void test_heavy_stretch_recovers(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.002, .b = 0.00012,      /* 9x: 11.7 ms (fits) */
                 .heavy_from = 10.0, .heavy_to = 30.0, .heavy_a = 0.008 };
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 30.0, 0.0);
    CHECK(r.level < 9, "stretch: still at %d during the heavy part", r.level);
    drive(&c, &r, &sc, 90.0, 0.0);
    CHECK(r.level == 9, "stretch: level %d after recovery", r.level);
    CHECK(r.downs <= 4, "stretch: %d downs", r.downs);
}

/* 9. Holds: nothing is sampled or decided while held. */
static void test_holds(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.003, .b = 0.00030 };
    ctl(&c, 3, 9, 9);
    run_init(&r, 9);
    drive(&c, &r, &sc, 5.0, 5.0);
    CHECK(r.steps == 0, "holds: %d steps while held", r.steps);
    dynrt_hold(&c, r.t, 1.0);
    drive(&c, &r, &sc, 0.9, 0.0);
    CHECK(r.steps == 0, "holds: %d steps in the hold tail", r.steps);
    drive(&c, &r, &sc, 2.0, 0.0);
    CHECK(r.downs >= 1, "holds: no step after the hold");
}

/* 10. Missing costs (frames drawn at sync points): thin windows, no step. */
static void test_thin_windows(void) {
    DynrtController c;
    ctl(&c, 3, 9, 9);
    double t = 0.0;
    for (int i = 0; i < 600; i++) {
        t += 0.02;
        DynrtSample s = { kPeriod, 0.02, 0, 0.0, 0.0, 0 };
        (void)dynrt_sample(&c, t, &s);
    }
    CHECK(c.level == 9 && c.downs == 0 && c.thin_windows > 10,
          "thin: level %d downs %llu thin %llu", c.level, c.downs, c.thin_windows);
}

/* 11. Force pins the level; release resumes. */
static void test_force(void) {
    DynrtController c; Run r;
    Scene sc = { .a = 0.003, .b = 0.00030 };
    ctl(&c, 3, 9, 9);
    CHECK(dynrt_force(&c, 7) == 7, "force: level");
    run_init(&r, 7);
    drive(&c, &r, &sc, 10.0, 0.0);
    CHECK(r.steps == 0, "force: %d steps while pinned", r.steps);
    (void)dynrt_force(&c, 0);
    drive(&c, &r, &sc, 10.0, 0.0);
    CHECK(r.level == fit_level(&sc, 3, 9), "force: released to %d", r.level);
}

int main(void) {
    test_overrun_steps_down_and_settles();
    test_headroom_steps_up_after_window();
    test_hysteresis();
    test_guest_bound_no_step();
    test_undo_and_backoff();
    test_queue_full_steps_down();
    test_floor_and_ceiling();
    test_heavy_stretch_recovers();
    test_holds();
    test_thin_windows();
    test_force();
    printf("dynamic_resolution_rt_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
