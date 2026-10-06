/* dynamic_resolution.h — the controller behind [video] dynamic_resolution
 * (docs/ENHANCEMENTS.md, IR3).
 *
 * Pure C: no SDL, no GL and no clock of its own, so a unit test drives it
 * with synthetic time (tests/test_dynamic_resolution.c). The host feeds it one
 * sample per guest interval (one VBlank) and applies the level it returns;
 * the GL backend changes the internal scale between frames
 * (gl_renderer_step_internal_scale_now).
 *
 * LOAD. A sample's work is the interval's wall time minus the host's idle
 * waits (wall-clock pacer, the frame blend's waits for its next present, the
 * driver's vsync block in the swap), render passes and the frame blend's extra
 * presents: emulation, submission, the frame's own present and any driver
 * blocking. load = work / the guest's nominal interval, averaged over a
 * window (0.5 s). In-between frames only spend idle time and the load leaves
 * them out, so when a scene gets heavier they are shed first; the resolution
 * drops only when the game's own frames would be late.
 *
 * LEVELS are the integer scales floor..ceiling. A level's load is predicted
 * from the current one as load * ((1 - f) + f * (S'/S)^2), f being the share
 * of the load that scales with the pixel count. f starts at a prior and is
 * learned from what each step actually changed.
 *
 * RULES (DynresParams; defaults in dynres_default_params):
 *  - down: a window at >= down_load with >= down_late late intervals, or two
 *    windows above down_load_sustained. Target: the highest level predicted at
 *    <= target_load (at least one lower). No second down step within
 *    cooldown_down_s unless a window has >= burst_late late intervals.
 *  - verify: the second window after a down step must show at least
 *    verify_fraction of the predicted fall. Otherwise the pressure was not the
 *    resolution (a CPU-bound stretch): the step is undone after verify_undo_s
 *    and down steps are blocked for verify_block_s, doubling per failure.
 *  - up: one level, after up_after_s of windows predicted at <= up_load at the
 *    next level with no late interval, the cooldown since the last step over,
 *    the level not blocked, and only at an interval whose idle time covers the
 *    measured step cost.
 *  - relapse: a level reached by an up step and left by a down step within
 *    relapse_s is blocked for up steps for relapse_block_s, doubling up to
 *    relapse_block_max_s; an up step that holds the level for
 *    relapse_forget_s resets that.
 *  - holds: the host's holds (turbo, loads, FMV, compiles, savestate loads,
 *    resizes, the first seconds of the game), and any interval longer than
 *    gap_factor intervals, discard the window in progress; nothing is sampled
 *    or decided until the hold's tail has passed. A single slow interval never
 *    steps: every decision needs a whole window.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define DYNRES_MAX_LEVEL 32

typedef struct DynresParams {
    double window_s;                 /* decision window, guest time */
    double late_factor;              /* interval > period * this is late */
    double gap_factor;               /* interval > period * this: a gap, held */
    double gap_hold_s;
    double down_load;                /* with down_late late intervals */
    int    down_late;
    double down_load_sustained;      /* two windows above it */
    double target_load;
    double cooldown_down_s;
    int    burst_late;
    double verify_fraction;
    double verify_undo_s;
    double verify_block_s, verify_block_max_s, verify_forget_s;
    double up_load;
    double up_after_s;
    double cooldown_up_after_up_s, cooldown_up_after_down_s;
    double relapse_s;
    double relapse_block_s, relapse_block_max_s, relapse_forget_s;
    double prior_scaled;             /* f before any step was measured */
    double learn_rate;
    double step_cost_s;              /* step cost before one was measured */
} DynresParams;

void dynres_default_params(DynresParams *p);

typedef struct DynresSample {
    double period_s;   /* the guest's nominal interval (1 / 59.94 for NTSC) */
    double wall_s;     /* this interval's wall time */
    double work_s;     /* see LOAD above */
    int    held;       /* the host holds: not a sample, the window restarts */
} DynresSample;

typedef struct DynresController {
    DynresParams p;
    int    floor, ceiling, level, forced;
    double f;                         /* learned share of load ~ S^2 */
    /* the window in progress */
    double win_period, win_work, win_wall;
    int    win_n, win_late;
    /* the last closed window */
    double last_load, last_vblank_hz;
    int    last_late, last_valid;
    double prev_load;                 /* the window before it (-1 = none) */
    double hold_until;
    double up_streak_s;
    int    up_ready;
    double last_step_t, last_down_t;
    int    last_step_up;
    double up_reached_t[DYNRES_MAX_LEVEL + 1];
    double up_block_until[DYNRES_MAX_LEVEL + 1];
    double relapse_dur[DYNRES_MAX_LEVEL + 1];
    double down_block_until, verify_dur, verify_last_fail;
    /* after a step: the window it is judged on */
    int    post_active, post_from, post_to, post_windows, post_down;
    double post_load_before, post_pred;
    int    undo_level;
    double undo_at;
    double step_cost_s;
    /* telemetry */
    unsigned long long downs, ups, undos, relapses, windows, held_windows;
    const char *last_reason;
    double last_decision_t;
} DynresController;

void dynres_init(DynresController *c, const DynresParams *p, int floor_level,
                 int ceiling, int level);
/* Hold sampling and decisions until now_s + tail_s (the latest hold wins). */
void dynres_hold(DynresController *c, double now_s, double tail_s);
/* One guest interval ending at now_s. Returns the level to render at (the
 * current one when nothing changes; the host applies a change). */
int  dynres_sample(DynresController *c, double now_s, const DynresSample *s);
/* The measured wall time of a step the host applied. */
void dynres_note_step_cost(DynresController *c, double seconds);
/* Pin a level (debugging); 0 releases the pin. Returns the level to apply. */
int  dynres_force(DynresController *c, int level);
/* Predicted load at level `to` from `load` measured at `from`. */
double dynres_predict(const DynresController *c, double load, int from, int to);
/* Up steps into `level` blocked for this many seconds from now_s (0 = not). */
double dynres_up_blocked_s(const DynresController *c, int level, double now_s);

#ifdef __cplusplus
}
#endif
