/* dynamic_resolution.c — see dynamic_resolution.h. */
#include "dynamic_resolution.h"

#include <string.h>

void dynres_default_params(DynresParams *p) {
    memset(p, 0, sizeof *p);
    p->window_s = 0.5;
    p->late_factor = 1.10;
    p->gap_factor = 4.0;
    p->gap_hold_s = 1.0;
    p->down_load = 0.90;
    p->down_late = 2;
    p->down_load_sustained = 0.92;
    p->target_load = 0.80;
    p->cooldown_down_s = 2.0;
    p->burst_late = 6;
    p->verify_fraction = 0.30;
    p->verify_undo_s = 1.0;
    p->verify_block_s = 30.0;
    p->verify_block_max_s = 480.0;
    p->verify_forget_s = 300.0;
    p->up_load = 0.75;
    p->up_after_s = 3.0;
    p->cooldown_up_after_up_s = 3.0;
    p->cooldown_up_after_down_s = 5.0;
    p->relapse_s = 10.0;
    p->relapse_block_s = 10.0;
    p->relapse_block_max_s = 160.0;
    p->relapse_forget_s = 60.0;
    p->prior_scaled = 0.6;
    p->learn_rate = 0.3;
    p->step_cost_s = 0.004;
}

static int clamp_level(const DynresController *c, int l) {
    if (l < c->floor) l = c->floor;
    if (l > c->ceiling) l = c->ceiling;
    return l;
}

void dynres_init(DynresController *c, const DynresParams *p, int floor_level,
                 int ceiling, int level) {
    memset(c, 0, sizeof *c);
    if (p) c->p = *p; else dynres_default_params(&c->p);
    if (ceiling < 1) ceiling = 1;
    if (ceiling > DYNRES_MAX_LEVEL) ceiling = DYNRES_MAX_LEVEL;
    if (floor_level < 1) floor_level = 1;
    if (floor_level > ceiling) floor_level = ceiling;
    c->floor = floor_level;
    c->ceiling = ceiling;
    c->level = clamp_level(c, level);
    c->f = c->p.prior_scaled;
    c->prev_load = -1.0;
    c->last_step_t = -1e9;
    c->last_down_t = -1e9;
    c->verify_last_fail = -1e9;
    c->verify_dur = c->p.verify_block_s;
    c->step_cost_s = c->p.step_cost_s;
    for (int i = 0; i <= DYNRES_MAX_LEVEL; i++) {
        c->up_reached_t[i] = -1e9;
        c->relapse_dur[i] = c->p.relapse_block_s;
    }
    c->last_reason = "start";
}

double dynres_predict(const DynresController *c, double load, int from, int to) {
    if (from < 1 || to < 1) return load;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    return load * ((1.0 - c->f) + c->f * q);
}

double dynres_up_blocked_s(const DynresController *c, int level, double now_s) {
    if (level < 0 || level > DYNRES_MAX_LEVEL) return 0.0;
    double r = c->up_block_until[level] - now_s;
    return r > 0.0 ? r : 0.0;
}

static void window_reset(DynresController *c) {
    c->win_period = c->win_work = c->win_wall = 0.0;
    c->win_n = c->win_late = 0;
}

/* Everything that needs consecutive clean windows starts over. */
static void discard(DynresController *c) {
    if (c->win_n) c->held_windows++;
    window_reset(c);
    c->prev_load = -1.0;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    /* A step's effect is judged on clean windows only: drop the judgement
     * (no learning, no undo) rather than blame a hold on the resolution. */
    c->post_active = 0;
}

void dynres_hold(DynresController *c, double now_s, double tail_s) {
    double until = now_s + (tail_s > 0.0 ? tail_s : 0.0);
    if (until > c->hold_until) c->hold_until = until;
    discard(c);
}

void dynres_note_step_cost(DynresController *c, double seconds) {
    if (seconds <= 0.0) return;
    c->step_cost_s = c->step_cost_s + 0.3 * (seconds - c->step_cost_s);
}

int dynres_force(DynresController *c, int level) {
    if (level <= 0) {
        c->forced = 0;
        c->last_reason = "force released";
        discard(c);
        return c->level;
    }
    c->forced = clamp_level(c, level);
    c->level = c->forced;
    c->post_active = 0;
    c->undo_level = 0;
    c->last_reason = "forced";
    discard(c);
    return c->level;
}

static void learn(DynresController *c, double before, double after, int from, int to) {
    if (before <= 0.05 || from == to) return;
    double q = ((double)to * (double)to) / ((double)from * (double)from);
    double fo = (after / before - 1.0) / (q - 1.0);
    if (fo < 0.1) fo = 0.1;
    if (fo > 0.95) fo = 0.95;
    c->f += c->p.learn_rate * (fo - c->f);
}

static void begin_post(DynresController *c, int from, int to, double load_before,
                       int down) {
    c->post_active = 1;
    c->post_from = from;
    c->post_to = to;
    c->post_windows = 0;
    c->post_down = down;
    c->post_load_before = load_before;
    c->post_pred = dynres_predict(c, load_before, from, to);
}

static int step_down(DynresController *c, double now, double load, const char *why) {
    int from = c->level, to = c->floor;
    for (int l = from - 1; l >= c->floor; l--)
        if (dynres_predict(c, load, from, l) <= c->p.target_load) { to = l; break; }
    /* Relapse: an up step into this level failed within relapse_s. */
    if (now - c->up_reached_t[from] < c->p.relapse_s) {
        c->up_block_until[from] = now + c->relapse_dur[from];
        c->relapse_dur[from] *= 2.0;
        if (c->relapse_dur[from] > c->p.relapse_block_max_s)
            c->relapse_dur[from] = c->p.relapse_block_max_s;
        c->relapses++;
    }
    c->up_reached_t[from] = -1e9;
    begin_post(c, from, to, load, 1);
    c->level = to;
    c->last_step_t = c->last_down_t = now;
    c->last_step_up = 0;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    c->prev_load = -1.0;
    c->downs++;
    c->last_reason = why;
    c->last_decision_t = now;
    return c->level;
}

static int step_up(DynresController *c, double now, double load, int to, const char *why) {
    int from = c->level;
    begin_post(c, from, to, load, 0);
    c->level = to;
    c->up_reached_t[to] = now;
    c->last_step_t = now;
    c->last_step_up = 1;
    c->up_streak_s = 0.0;
    c->up_ready = 0;
    c->prev_load = -1.0;
    c->ups++;
    c->last_reason = why;
    c->last_decision_t = now;
    return c->level;
}

/* A window closed: judge a recent step, then the rules. */
static int close_window(DynresController *c, double now) {
    const double load = c->win_work / c->win_period;
    const int late = c->win_late;
    const double dur = c->win_period;
    c->last_load = load;
    c->last_late = late;
    c->last_vblank_hz = c->win_wall > 0.0 ? (double)c->win_n / c->win_wall : 0.0;
    c->last_valid = 1;
    c->windows++;
    window_reset(c);

    if (c->post_active && ++c->post_windows >= 2) {
        /* The first window after a step settles; the second judges it. */
        c->post_active = 0;
        double fell = c->post_load_before - load;
        double want = c->post_load_before - c->post_pred;
        if (c->post_down && want > 0.0 && fell < c->p.verify_fraction * want) {
            /* Not resolution-bound: undo, and block down steps a while. */
            if (now - c->verify_last_fail > c->p.verify_forget_s)
                c->verify_dur = c->p.verify_block_s;
            c->down_block_until = now + c->verify_dur;
            c->verify_dur *= 2.0;
            if (c->verify_dur > c->p.verify_block_max_s)
                c->verify_dur = c->p.verify_block_max_s;
            c->verify_last_fail = now;
            c->undo_level = c->post_from;
            c->undo_at = now + c->p.verify_undo_s;
            c->last_reason = "down step did not lower the load (not resolution-bound)";
            c->last_decision_t = now;
        } else {
            learn(c, c->post_load_before, load, c->post_from, c->post_to);
        }
    }
    if (c->undo_level && now >= c->undo_at) {
        int to = clamp_level(c, c->undo_level);
        c->undo_level = 0;
        if (to != c->level) {
            c->undos++;
            c->level = to;
            c->last_step_t = now;
            c->last_step_up = 1;
            c->prev_load = -1.0;
            c->up_streak_s = 0.0;
            c->up_ready = 0;
            c->last_reason = "undo";
            c->last_decision_t = now;
            return c->level;
        }
    }

    /* A level an up step reached and kept for relapse_forget_s is stable:
     * its next relapse starts the back-off over. */
    if (now - c->up_reached_t[c->level] >= c->p.relapse_forget_s) {
        c->relapse_dur[c->level] = c->p.relapse_block_s;
        c->up_reached_t[c->level] = -1e9;
    }
    const double prev = c->prev_load;
    c->prev_load = load;
    if (c->post_active || c->undo_level) return c->level;   /* judging a step */

    int over = (load >= c->p.down_load && late >= c->p.down_late) ||
               (load > c->p.down_load_sustained && prev > c->p.down_load_sustained);
    if (over) {
        c->up_streak_s = 0.0;
        c->up_ready = 0;
        if (c->level > c->floor && now >= c->down_block_until &&
            (now - c->last_down_t >= c->p.cooldown_down_s || late >= c->p.burst_late))
            return step_down(c, now, load,
                             late >= c->p.down_late ? "busy with late frames"
                                                    : "busy for two windows");
        return c->level;
    }
    if (c->level >= c->ceiling) { c->up_streak_s = 0.0; c->up_ready = 0; return c->level; }
    const int next = c->level + 1;
    if (late == 0 && dynres_predict(c, load, c->level, next) <= c->p.up_load)
        c->up_streak_s += dur;
    else {
        c->up_streak_s = 0.0;
        c->up_ready = 0;
    }
    double cd = c->last_step_up ? c->p.cooldown_up_after_up_s : c->p.cooldown_up_after_down_s;
    c->up_ready = c->up_streak_s >= c->p.up_after_s - 1e-9 &&
                  now - c->last_step_t >= cd &&
                  now >= c->up_block_until[next];
    return c->level;
}

int dynres_sample(DynresController *c, double now_s, const DynresSample *s) {
    if (c->forced) return c->level;
    if (!s || s->period_s <= 0.0) return c->level;
    if (s->held || now_s < c->hold_until) {
        if (s->held) discard(c);
        else if (c->win_n) discard(c);
        return c->level;
    }
    if (s->wall_s > s->period_s * c->p.gap_factor) {
        /* A pause, a stall in the host, a window drag: not the scene. */
        dynres_hold(c, now_s, c->p.gap_hold_s);
        c->last_reason = "gap";
        return c->level;
    }
    double work = s->work_s < 0.0 ? 0.0 : s->work_s;
    if (c->up_ready) {
        /* Up steps land in an interval whose idle time covers the step. */
        double idle = s->period_s - work;
        if (idle >= c->step_cost_s && c->level < c->ceiling)
            return step_up(c, now_s, c->last_load, c->level + 1, "headroom");
    }
    c->win_period += s->period_s;
    c->win_work += work;
    c->win_wall += s->wall_s;
    c->win_n++;
    if (s->wall_s > s->period_s * c->p.late_factor) c->win_late++;
    if (c->win_period >= c->p.window_s - 1e-9) return close_window(c, now_s);
    return c->level;
}
