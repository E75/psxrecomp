/* The dynamic-resolution controller (dynamic_resolution.c) against synthetic
 * guest load: a scene costs base + k * S^2 per interval (S the level), and an
 * interval lasts max(period, work) like a wall-paced frame. Checks the step
 * rules, hysteresis (no oscillation), that transient spikes and CPU-bound
 * stretches never cost resolution for long, holds, the floor and pins.
 * Build/run: ctest -R dynamic_resolution_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dynamic_resolution.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
    failures++; } } while (0)

static const double kPeriod = 1.0 / 59.94;

/* Load model: the load (work / period) at level S at time t. */
typedef double (*LoadFn)(int level, double t, void *ctx);

typedef struct Run {
    double t;
    int    level;
    int    steps, downs, ups;
    int    min_level, max_level;
    double time_at[DYNRES_MAX_LEVEL + 1];
    double last_up_t[64];
    int    up_to[64];
    int    n_up;
} Run;

static void run_init(Run *r, int level) {
    memset(r, 0, sizeof *r);
    r->level = level;
    r->min_level = r->max_level = level;
}

/* Drive c for `seconds` of guest intervals. held_until: the host holds every
 * interval before it. */
static void simulate(DynresController *c, Run *r, double seconds, LoadFn fn, void *ctx,
                     double held_until) {
    double end = r->t + seconds;
    while (r->t < end) {
        double load = fn(r->level, r->t, ctx);
        double work = load * kPeriod;
        double wall = work > kPeriod ? work : kPeriod;
        r->t += wall;
        DynresSample s = { kPeriod, wall, work, r->t < held_until };
        int l = dynres_sample(c, r->t, &s);
        r->time_at[r->level] += wall;
        if (l != r->level) {
            r->steps++;
            if (l < r->level) r->downs++;
            else if (r->n_up < 64) {
                r->ups++;
                r->up_to[r->n_up] = l;
                r->last_up_t[r->n_up++] = r->t;
            } else {
                r->ups++;
            }
            r->level = l;
            if (l < r->min_level) r->min_level = l;
            if (l > r->max_level) r->max_level = l;
            dynres_note_step_cost(c, 0.002);
        }
    }
}

/* base + k*S^2 */
typedef struct Quad { double base, k; } Quad;
static double quad_load(int s, double t, void *ctx) {
    (void)t;
    const Quad *q = (const Quad *)ctx;
    return q->base + q->k * (double)s * (double)s;
}

static DynresController make(int floor, int ceiling) {
    DynresController c;
    dynres_init(&c, NULL, floor, ceiling, ceiling);
    return c;
}

static void test_light_scene_stays_at_ceiling(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.30, 0.004 };            /* 0.70 at 10x */
    simulate(&c, &r, 120.0, quad_load, &q, 0.0);
    CHECK(r.steps == 0 && r.level == 10, "light scene: %d steps, level %d", r.steps, r.level);
}

static void test_heavy_scene_settles_without_oscillation(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.35, 0.0065 };           /* 10x 1.00, 9x 0.88, 8x 0.77, 7x 0.67 */
    simulate(&c, &r, 300.0, quad_load, &q, 0.0);
    double load = quad_load(r.level, 0, &q);
    CHECK(r.level < 10 && load <= 0.90, "heavy scene: settles below the ceiling (level %d, load %.2f)",
          r.level, load);
    CHECK(r.level >= 7, "heavy scene: no deeper than needed (level %d)", r.level);
    CHECK(r.steps <= 6, "heavy scene: %d steps in 300 s (oscillation)", r.steps);
    /* After the first 60 s it holds still. */
    int before = r.steps;
    simulate(&c, &r, 300.0, quad_load, &q, 0.0);
    CHECK(r.steps - before <= 1, "heavy scene: %d steps in the next 300 s", r.steps - before);
    CHECK(c.downs >= 1, "heavy scene: stepped down");
}

/* A level whose predicted load sits just above the up threshold at the next
 * level never flips back up. */
static void test_hysteresis_band(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.40, 0.0055 };           /* 10x 0.95, 9x 0.85, 8x 0.75 */
    simulate(&c, &r, 600.0, quad_load, &q, 0.0);
    CHECK(r.downs >= 1, "band: stepped down from 0.95");
    CHECK(r.ups <= r.downs, "band: ups %d downs %d", r.ups, r.downs);
    CHECK(r.steps <= 5, "band: %d steps in 600 s", r.steps);
    CHECK(r.level <= 9, "band: stays below the ceiling (level %d)", r.level);
}

/* The model says the next level fits but it does not: the relapse back-off
 * makes each retry wait longer. */
typedef struct Liar { double at_low, at_high; int high; } Liar;
static double liar_load(int s, double t, void *ctx) {
    (void)t;
    const Liar *l = (const Liar *)ctx;
    return s >= l->high ? l->at_high : l->at_low;
}
static void test_relapse_backoff(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 9);
    c.level = 9;
    Liar l = { 0.40, 0.97, 10 };         /* 9x is light, 10x is far over */
    simulate(&c, &r, 900.0, liar_load, &l, 0.0);
    /* Retries of the failing level: their gaps grow (blocks of 10, 20, 40,
     * ... up to 160 s). */
    double at[64];
    int n = 0;
    for (int i = 0; i < r.n_up; i++)
        if (r.up_to[i] == 10) at[n++] = r.last_up_t[i];
    CHECK(n >= 2, "relapse: retried the ceiling (%d retries)", n);
    CHECK(c.relapses >= 2, "relapse: %llu relapses", c.relapses);
    int grew = 1;
    for (int i = 2; i < n; i++) {
        double a = at[i - 1] - at[i - 2], b = at[i] - at[i - 1];
        if (b + 1.0 < a || (a < 150.0 && b < 1.5 * a)) grew = 0;
    }
    CHECK(grew, "relapse: retries back off");
    CHECK(n <= 9, "relapse: %d retries in 900 s", n);
    CHECK(r.time_at[10] < 0.10 * 900.0, "relapse: %.0f s at the failing level", r.time_at[10]);
}

/* CPU-bound: the load does not depend on the level. Down steps are undone and
 * blocked, so the resolution is rarely lowered. */
static double cpu_bound(int s, double t, void *ctx) { (void)s; (void)t; (void)ctx; return 0.97; }
static void test_cpu_bound_keeps_resolution(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    simulate(&c, &r, 600.0, cpu_bound, NULL, 0.0);
    double below = 600.0 - r.time_at[10];
    CHECK(c.undos >= 1, "cpu-bound: a down step was undone (%llu)", c.undos);
    CHECK(below < 0.05 * 600.0, "cpu-bound: %.1f s of 600 below the ceiling", below);
    CHECK(r.downs <= 6, "cpu-bound: %d down steps in 600 s", r.downs);
    CHECK(r.level == 10, "cpu-bound: ends at the ceiling (level %d)", r.level);
}

/* Transients: one 100 ms stall every 7 s, and three slow intervals every 5 s,
 * in a scene at 0.70: never a step. */
static double spiky(int s, double t, void *ctx) {
    (void)s; (void)ctx;
    double m7 = fmod(t, 7.0), m5 = fmod(t, 5.0), m11 = fmod(t, 11.0);
    if (m7 < kPeriod) return 0.1 / kPeriod;            /* one 100 ms interval */
    if (m11 < 0.3) return 0.15 / kPeriod;              /* two 150 ms hitches */
    if (m5 < 3.0 * kPeriod * 1.6) return 1.6;          /* ~3 late intervals */
    return 0.70;
}
static void test_transient_spikes_ignored(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    simulate(&c, &r, 300.0, spiky, NULL, 0.0);
    CHECK(r.steps == 0, "spikes: %d steps", r.steps);
}

/* Busy but on time: no late frame, under the two-window bar. */
static double busy_on_time(int s, double t, void *ctx) { (void)s; (void)t; (void)ctx; return 0.91; }
static void test_busy_on_time_keeps_resolution(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    simulate(&c, &r, 120.0, busy_on_time, NULL, 0.0);
    CHECK(r.steps == 0, "busy on time: %d steps", r.steps);
}

/* Headroom must last up_after_s: 2 s light, then half a second heavy. */
static double bursty(int s, double t, void *ctx) {
    (void)ctx;
    return fmod(t, 2.5) < 2.0 ? 0.30 : (s >= 10 ? 0.99 : 0.95);
}
static void test_up_needs_sustained_headroom(void) {
    DynresController c = make(3, 10);
    c.level = 9;
    c.f = 0.2;
    Run r; run_init(&r, 9);
    simulate(&c, &r, 120.0, bursty, NULL, 0.0);
    CHECK(r.ups == 0, "bursty: %d up steps on 2 s of headroom", r.ups);
}

/* The scene gets lighter: back up, one level at a time, >= 3 s apart. */
typedef struct Phase { double switch_t; Quad heavy, light; } Phase;
static double phased(int s, double t, void *ctx) {
    const Phase *p = (const Phase *)ctx;
    return quad_load(s, t, t < p->switch_t ? (void *)&p->heavy : (void *)&p->light);
}
static void test_recovers_one_level_at_a_time(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Phase p = { 60.0, { 0.45, 0.0090 }, { 0.20, 0.0030 } };   /* heavy: 7x 0.89, 6x 0.77 */
    simulate(&c, &r, 60.0, phased, &p, 0.0);
    int low = r.level;
    CHECK(low <= 7, "phased: heavy part steps down (level %d)", low);
    int ups0 = r.ups;
    simulate(&c, &r, 120.0, phased, &p, 0.0);
    CHECK(r.level == 10, "phased: back to the ceiling (level %d)", r.level);
    CHECK(r.ups - ups0 == 10 - low, "phased: %d up steps for %d levels", r.ups - ups0, 10 - low);
    int spaced = 1;
    for (int i = 1; i < r.n_up; i++)
        if (r.last_up_t[i] - r.last_up_t[i - 1] < 3.0) spaced = 0;
    CHECK(spaced, "phased: up steps at least 3 s apart");
}

static void test_holds(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.35, 0.0065 };
    simulate(&c, &r, 30.0, quad_load, &q, 30.0);      /* held throughout */
    CHECK(r.steps == 0, "hold: %d steps while held", r.steps);
    dynres_hold(&c, r.t, 5.0);                        /* a tail */
    simulate(&c, &r, 4.5, quad_load, &q, 0.0);
    CHECK(r.steps == 0, "hold: %d steps inside the tail", r.steps);
    simulate(&c, &r, 10.0, quad_load, &q, 0.0);
    CHECK(r.downs >= 1, "hold: steps once the tail has passed");
}

static void test_floor_and_inert(void) {
    DynresController c = make(8, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.6, 0.008 };                         /* over even at 8x */
    simulate(&c, &r, 120.0, quad_load, &q, 0.0);
    CHECK(r.min_level >= 8, "floor: never below 8 (min %d)", r.min_level);
    DynresController d = make(5, 5);
    Run r2; run_init(&r2, 5);
    simulate(&d, &r2, 60.0, quad_load, &q, 0.0);
    CHECK(r2.steps == 0, "floor == ceiling: inert (%d steps)", r2.steps);
}

static void test_force(void) {
    DynresController c = make(3, 10);
    CHECK(dynres_force(&c, 4) == 4 && c.level == 4, "force: pins 4");
    Run r; run_init(&r, 4);
    Quad q = { 0.2, 0.002 };
    simulate(&c, &r, 30.0, quad_load, &q, 0.0);
    CHECK(r.steps == 0 && r.level == 4, "force: stays pinned");
    CHECK(dynres_force(&c, 99) == 10, "force: clamps to the ceiling");
    dynres_force(&c, 0);
    CHECK(!c.forced, "force: released");
}

/* Up steps wait for an interval whose idle time covers the step. */
typedef struct Tight { int n; } Tight;
static double tight(int s, double t, void *ctx) {
    (void)s; (void)t;
    Tight *k = (Tight *)ctx;
    k->n++;
    /* Mostly 0.30, but every interval leaves less than 4 ms idle until
     * n passes 60 s worth; then one roomy interval. */
    return 0.80;
}
static void test_up_waits_for_idle(void) {
    DynresController c = make(3, 10);
    c.level = 9;
    c.f = 0.0;                                       /* the next level predicts 0.80 * 1 */
    c.p.up_load = 0.85;
    c.step_cost_s = 0.005;                           /* idle at 0.80 is 3.3 ms */
    Run r; run_init(&r, 9);
    Tight k = { 0 };
    simulate(&c, &r, 30.0, tight, &k, 0.0);
    CHECK(r.ups == 0, "idle: no up step without room for it (%d)", r.ups);
    c.step_cost_s = 0.003;
    simulate(&c, &r, 10.0, tight, &k, 0.0);
    CHECK(r.ups == 1, "idle: up step once it fits (%d)", r.ups);
}

/* f is learned from what steps change. */
static void test_learns_scaling(void) {
    DynresController c = make(3, 10);
    Run r; run_init(&r, 10);
    Quad q = { 0.05, 0.0092 };                       /* nearly all ~S^2: f ~ 0.95 */
    simulate(&c, &r, 120.0, quad_load, &q, 0.0);
    CHECK(c.f > c.p.prior_scaled, "learn: f rose from %.2f to %.2f", c.p.prior_scaled, c.f);
}

/* ---- Render passes (frame-rate priority) ---------------------------------
 * The game's load at level S is base + k*S^2; the render passes want
 * demand(S) of every interval and get what the leftover planner admits
 * (share of what the game leaves); the rest is shed. Passes run in the
 * leftover time, so an interval stays one period long. */
typedef struct PassScene {
    double base, k;           /* game load */
    double d0, dk;            /* pass demand: d0 + dk*S^2 */
    double share;             /* of the leftover */
} PassScene;

typedef struct PassRun {
    Run    r;
    double shed_last;         /* shed load at the current level */
} PassRun;

static void pass_simulate(DynresController *c, PassRun *pr, double seconds,
                          const PassScene *ps) {
    Run *r = &pr->r;
    double end = r->t + seconds;
    while (r->t < end) {
        double S2 = (double)r->level * (double)r->level;
        double load = ps->base + ps->k * S2;
        double demand = ps->d0 + ps->dk * S2;
        double left = 1.0 - load;
        double budget = left > 0.0 ? left * ps->share : 0.0;
        double run = demand < budget ? demand : budget;
        double shed = demand - run;
        double work = load * kPeriod;
        double wall = work > kPeriod ? work : kPeriod;
        r->t += wall;
        pr->shed_last = shed;
        DynresSample s = { kPeriod, wall, work, 0, run * kPeriod, shed * kPeriod, 0.0, 0 };
        int l = dynres_sample(c, r->t, &s);
        r->time_at[r->level] += wall;
        if (l != r->level) {
            r->steps++;
            if (l < r->level) r->downs++; else r->ups++;
            r->level = l;
            if (l < r->min_level) r->min_level = l;
            if (l > r->max_level) r->max_level = l;
            dynres_note_step_cost(c, 0.002);
        }
    }
}

static DynresController make_prio(int floor, int ceiling, int priority) {
    DynresParams p;
    DynresController c;
    dynres_default_params(&p);
    p.priority = priority;
    dynres_init(&c, &p, floor, ceiling, ceiling);
    return c;
}

/* The default priority never trades resolution for passes. */
static void test_resolution_priority_ignores_shedding(void) {
    DynresController c = make_prio(3, 10, DYNRES_PRIORITY_RESOLUTION);
    PassRun pr; memset(&pr, 0, sizeof pr); run_init(&pr.r, 10);
    PassScene ps = { 0.30, 0.002, 0.0, 0.004, 0.65 };   /* 10x: game 0.50, passes want 0.40 */
    pass_simulate(&c, &pr, 120.0, &ps);
    CHECK(pr.r.steps == 0 && pr.r.level == 10,
          "resolution priority: %d steps, level %d (passes shed %.2f)",
          pr.r.steps, pr.r.level, pr.shed_last);
}

/* Frame-rate priority: sustained shedding steps down, one level at a time,
 * until the passes fit, and the level then holds still. */
static void test_frame_rate_priority_makes_room(void) {
    DynresController c = make_prio(3, 10, DYNRES_PRIORITY_FRAME_RATE);
    PassRun pr; memset(&pr, 0, sizeof pr); run_init(&pr.r, 10);
    PassScene ps = { 0.30, 0.002, 0.0, 0.004, 0.65 };
    /* 10x: 0.50 game, 0.40 wanted, 0.325 admitted (0.075 shed);
     * 9x: 0.462/0.324/0.35 -> nothing shed. */
    pass_simulate(&c, &pr, 60.0, &ps);
    CHECK(pr.r.level < 10, "frame rate: stepped down for the passes (level %d)", pr.r.level);
    CHECK(pr.r.level >= 8, "frame rate: no deeper than needed (level %d)", pr.r.level);
    CHECK(pr.shed_last < 0.02, "frame rate: passes fit (shed %.3f)", pr.shed_last);
    CHECK(c.shed_downs >= 1, "frame rate: counted as a shed down step");
    int before = pr.r.steps;
    pass_simulate(&c, &pr, 600.0, &ps);
    CHECK(pr.r.steps - before <= 4, "frame rate: %d steps in the next 600 s (oscillation)",
          pr.r.steps - before);
    CHECK(pr.r.time_at[10] < 60.0, "frame rate: %.1f s back at the shedding level",
          pr.r.time_at[10]);
}

/* Never below the floor, however much the passes want. */
static void test_frame_rate_priority_floor(void) {
    DynresController c = make_prio(6, 10, DYNRES_PRIORITY_FRAME_RATE);
    PassRun pr; memset(&pr, 0, sizeof pr); run_init(&pr.r, 10);
    PassScene ps = { 0.30, 0.002, 0.0, 0.03, 0.65 };   /* shed at every level */
    pass_simulate(&c, &pr, 300.0, &ps);
    CHECK(pr.r.min_level >= 6, "floor: min level %d", pr.r.min_level);
}

/* Shedding the resolution does not cure (a demand that does not scale with
 * the pixel count) is undone, and shed down steps back off. */
static void test_frame_rate_priority_not_resolution_bound(void) {
    DynresController c = make_prio(3, 10, DYNRES_PRIORITY_FRAME_RATE);
    PassRun pr; memset(&pr, 0, sizeof pr); run_init(&pr.r, 10);
    PassScene ps = { 0.30, 0.002, 0.60, 0.0, 0.65 };   /* passes want 0.6 at any level */
    pass_simulate(&c, &pr, 600.0, &ps);
    CHECK(c.undos >= 1, "not resolution-bound: undone (%llu)", c.undos);
    CHECK(pr.r.time_at[10] > 480.0, "not resolution-bound: %.0f s of 600 at the ceiling",
          pr.r.time_at[10]);
    CHECK(pr.r.steps <= 12, "not resolution-bound: %d steps in 600 s", pr.r.steps);
}

/* Up steps wait while passes are shed (frame-rate priority), and need the
 * passes' demand to fit at the next level; the default priority steps up on
 * the game's load alone. */
static void test_frame_rate_priority_blocks_up(void) {
    PassScene ps = { 0.20, 0.002, 0.0, 0.005, 0.65 };
    /* 9x: game 0.36, passes want 0.405, admitted 0.416 -> fit;
     * 10x: game 0.40, want 0.50, admitted 0.39 -> shed. Game alone fits 10x. */
    DynresController c = make_prio(3, 10, DYNRES_PRIORITY_FRAME_RATE);
    c.level = 9;
    PassRun pr; memset(&pr, 0, sizeof pr); run_init(&pr.r, 9);
    pass_simulate(&c, &pr, 300.0, &ps);
    CHECK(pr.r.ups == 0 && pr.r.level == 9,
          "frame rate: no up step into shedding (ups %d, level %d)", pr.r.ups, pr.r.level);
    DynresController d = make_prio(3, 10, DYNRES_PRIORITY_RESOLUTION);
    d.level = 9;
    PassRun pq; memset(&pq, 0, sizeof pq); run_init(&pq.r, 9);
    pass_simulate(&d, &pq, 300.0, &ps);
    CHECK(pq.r.level == 10, "resolution: steps up on the game's load (level %d)", pq.r.level);
    /* Shedding a little at 9x (above shed_up_max) blocks the up step too. */
    DynresController e = make_prio(3, 10, DYNRES_PRIORITY_FRAME_RATE);
    e.level = 9;
    int ups = 0;
    double t = 0.0;
    for (int i = 0; i < 300 * 60; i++) {
        t += kPeriod;
        DynresSample s = { kPeriod, kPeriod, 0.30 * kPeriod, 0, 0.0, 0.05 * kPeriod, 0.0, 0 };
        if (dynres_sample(&e, t, &s) > 9) ups++;
    }
    CHECK(ups == 0, "frame rate: up steps while shedding: %d", ups);
}

/* Swap time left out as vsync wait is work in a window in which the pace
 * guard cut passes (the guest slipped: the swap waited on the GPU). */
static void test_swap_counts_after_pace_cut(void) {
    for (int cut = 0; cut <= 1; cut++) {
        DynresController c = make_prio(3, 10, DYNRES_PRIORITY_RESOLUTION);
        double t = 0.0;
        int level = 10;
        for (int i = 0; i < 10 * 60; i++) {
            t += kPeriod;
            /* 0.60 of measured work, 0.35 blocked in the swap */
            DynresSample s = { kPeriod, kPeriod, 0.60 * kPeriod, 0, 0.0, 0.0,
                               0.35 * kPeriod, cut && (i % 4) == 0 };
            level = dynres_sample(&c, t, &s);
            if (level < 10) break;
        }
        if (cut)
            CHECK(level < 10 && c.pace_cut_windows >= 1,
                  "pace cut: swap counted (level %d, load %.2f)", level, c.last_load);
        else
            CHECK(level == 10 && c.last_load < 0.7,
                  "no pace cut: swap is idle (level %d, load %.2f)", level, c.last_load);
    }
}

int main(void) {
    test_light_scene_stays_at_ceiling();
    test_heavy_scene_settles_without_oscillation();
    test_hysteresis_band();
    test_relapse_backoff();
    test_cpu_bound_keeps_resolution();
    test_transient_spikes_ignored();
    test_busy_on_time_keeps_resolution();
    test_up_needs_sustained_headroom();
    test_recovers_one_level_at_a_time();
    test_holds();
    test_floor_and_inert();
    test_force();
    test_up_waits_for_idle();
    test_learns_scaling();
    test_resolution_priority_ignores_shedding();
    test_frame_rate_priority_makes_room();
    test_frame_rate_priority_floor();
    test_frame_rate_priority_not_resolution_bound();
    test_frame_rate_priority_blocks_up();
    test_swap_counts_after_pace_cut();
    printf("dynamic_resolution_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
