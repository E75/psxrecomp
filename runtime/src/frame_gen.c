/* Frame generation: matching, interpolation, planning (include/frame_gen.h,
 * docs/FRAME_GENERATION.md). */
#include "frame_gen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void fg_prims_reset(FgPrimList *l) { l->n = 0; }

void fg_prims_free(FgPrimList *l) {
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

int fg_prims_add(FgPrimList *l, const FgPrim *p) {
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2u : 1024u;
        FgPrim *nv = (FgPrim *)realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = *p;
    return 1;
}

uint32_t fg_hash(uint32_t h, const int32_t *w, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t x = (uint32_t)w[i];
        for (int b = 0; b < 4; b++) {
            h ^= (x >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

void fg_match_defaults(FgMatchParams *p) {
    p->max_move = 96.0f;
    p->max_deform = 24.0f;
    p->window = 16;
}

/* Open-addressing table from key to the first older primitive with it; the
 * older primitives with one key are chained in draw order. */
typedef struct {
    uint32_t *keys;
    int32_t  *head;
    uint32_t  mask;
} KeyTab;

static int tab_init(KeyTab *t, uint32_t n) {
    uint32_t cap = 64;
    while (cap < n * 2u) cap <<= 1;
    t->keys = (uint32_t *)malloc((size_t)cap * sizeof *t->keys);
    t->head = (int32_t *)malloc((size_t)cap * sizeof *t->head);
    t->mask = cap - 1;
    if (!t->keys || !t->head) return 0;
    for (uint32_t i = 0; i < cap; i++) t->head[i] = -1;
    return 1;
}

static int32_t *tab_slot(KeyTab *t, uint32_t key) {
    uint32_t i = (key * 2654435761u) & t->mask;
    while (t->head[i] >= 0 && t->keys[i] != key) i = (i + 1) & t->mask;
    t->keys[i] = key;
    return &t->head[i];
}

uint32_t fg_match(const FgPrimList *older, const FgPrimList *newer,
                  const FgMatchParams *p, int32_t *match, FgMatchStats *st) {
    FgMatchStats s;
    memset(&s, 0, sizeof s);
    s.prims = newer->n;
    for (uint32_t i = 0; i < newer->n; i++) match[i] = -1;
    if (!older->n || !newer->n) {
        s.unmatched = newer->n;
        if (st) *st = s;
        return 0;
    }
    KeyTab t;
    int32_t *next = (int32_t *)malloc((size_t)older->n * sizeof *next);
    uint8_t *used = (uint8_t *)calloc(older->n, 1);
    if (!tab_init(&t, older->n) || !next || !used) {
        free(t.keys); free(t.head); free(next); free(used);
        s.unmatched = newer->n;
        if (st) *st = s;
        return 0;
    }
    /* Chains in draw order: insert from the back. */
    for (uint32_t i = older->n; i-- > 0;) {
        int32_t *h = tab_slot(&t, older->v[i].key);
        next[i] = *h;
        *h = (int32_t)i;
    }
    const float mm = p->max_move * p->max_move;
    const float md = p->max_deform * p->max_deform;
    for (uint32_t j = 0; j < newer->n; j++) {
        const FgPrim *b = &newer->v[j];
        int32_t *h = tab_slot(&t, b->key);
        /* Drop used entries at the chain head so later lookups stay short. */
        while (*h >= 0 && used[*h]) *h = next[*h];
        int32_t best = -1;
        float best_d = 0.0f;
        int seen = 0;
        for (int32_t i = *h; i >= 0 && seen < p->window; i = next[i]) {
            if (used[i]) continue;
            seen++;
            const FgPrim *a = &older->v[i];
            float dx[3], dy[3], d = 0.0f;
            int ok = 1;
            for (int k = 0; k < 3 && ok; k++) {
                dx[k] = b->x[k] - a->x[k];
                dy[k] = b->y[k] - a->y[k];
                float d2 = dx[k] * dx[k] + dy[k] * dy[k];
                if (d2 > mm) ok = 0;
                d += d2;
            }
            for (int k = 1; k < 3 && ok; k++) {
                float ex = dx[k] - dx[0], ey = dy[k] - dy[0];
                if (ex * ex + ey * ey > md) ok = 0;
            }
            if (!ok) continue;
            if (best < 0 || d < best_d) {
                best = i;
                best_d = d;
                if (d == 0.0f) break;   /* did not move: nothing closer */
            }
        }
        if (best >= 0) {
            used[best] = 1;
            match[j] = best;
            s.matched++;
            if (best_d > 0.0f) s.moved++;
        } else {
            s.unmatched++;
        }
    }
    free(t.keys); free(t.head); free(next); free(used);
    if (st) *st = s;
    return s.matched;
}

void fg_lerp(const FgPrim *a, const FgPrim *b, double t, float x[3], float y[3]) {
    for (int k = 0; k < 3; k++) {
        if (t >= 1.0) { x[k] = b->x[k]; y[k] = b->y[k]; continue; }
        if (t <= 0.0) { x[k] = a->x[k]; y[k] = a->y[k]; continue; }
        x[k] = (float)((double)a->x[k] + ((double)b->x[k] - (double)a->x[k]) * t);
        y[k] = (float)((double)a->y[k] + ((double)b->y[k] - (double)a->y[k]) * t);
    }
}

int fg_plan(double flip_s, double refresh_hz, double real_cost_s,
            double gen_cost_s, double budget, int max_gens) {
    if (flip_s <= 0.0 || refresh_hz <= 0.0 || max_gens <= 0) return 0;
    int slots = (int)floor(flip_s * refresh_hz + 0.5);
    if (slots < 2) return 0;
    int n = slots - 1;
    if (n > max_gens) n = max_gens;
    const double room = budget * flip_s - (real_cost_s > 0.0 ? real_cost_s : 0.0);
    if (room <= 0.0) return 0;
    if (gen_cost_s <= 0.0) return room >= 0.5 * budget * flip_s ? 1 : 0;
    int fit = (int)floor(room / gen_cost_s);
    return fit < n ? (fit > 0 ? fit : 0) : n;
}

void fg_breaker_init(FgBreaker *b, double base_hold, double max_hold, double repeat_s) {
    memset(b, 0, sizeof *b);
    b->base_hold = base_hold;
    b->max_hold = max_hold;
    b->repeat_s = repeat_s;
    b->last_end = -1e30;
    b->until = -1e30;
}

void fg_breaker_trip(FgBreaker *b, double now, const char *reason) {
    b->trips++;
    b->reason = reason;
    if (now < b->until) return;   /* already open: the hold stands */
    if (b->hold > 0.0 && now - b->last_end <= b->repeat_s) {
        b->hold *= 2.0;
        if (b->hold > b->max_hold) b->hold = b->max_hold;
    } else {
        b->hold = b->base_hold;
    }
    b->until = now + b->hold;
    b->last_end = b->until;
}

int fg_breaker_open(const FgBreaker *b, double now) { return now >= b->until; }

void fg_cost_init(FgCost *c, double probe_s, double max_probe_s) {
    memset(c, 0, sizeof *c);
    c->base_probe_s = c->probe_s = probe_s;
    c->max_probe_s = max_probe_s;
    c->blocked_since = -1.0;
}

void fg_cost_cold(FgCost *c, int n) { if (n > c->cold) c->cold = n; }

void fg_cost_add(FgCost *c, double cost_s, double fit_s) {
    if (cost_s <= 0.0) return;
    if (c->cold > 0) { c->cold--; c->discarded++; return; }
    c->samples++;
    if (c->probing || c->ema <= 0.0) {
        c->ema = cost_s;
        if (c->probing) {
            c->probing = 0;
            if (fit_s > 0.0 && cost_s > fit_s) {
                c->probe_s *= 2.0;
                if (c->probe_s > c->max_probe_s) c->probe_s = c->max_probe_s;
            } else {
                c->probe_s = c->base_probe_s;
            }
        }
    } else {
        c->ema = c->ema * 0.8 + cost_s * 0.2;
    }
    if (fit_s <= 0.0 || c->ema <= fit_s) c->blocked_since = -1.0;
}

double fg_cost_estimate(FgCost *c, double now, double fit_s) {
    if (c->ema <= 0.0) return 0.0;
    if (c->probing) {
        /* A probe in flight; one never drawn (no room) is given up. */
        if (now - c->probe_at < c->probe_s) return c->ema;
        c->probing = 0;
        c->blocked_since = now;
    }
    if (fit_s > 0.0 && c->ema > fit_s) {
        if (c->blocked_since < 0.0) c->blocked_since = now;
        else if (now - c->blocked_since >= c->probe_s) {
            c->blocked_since = now;
            c->probing = 1;
            c->probe_at = now;
            c->probes++;
            return 0.0;
        }
    } else {
        c->blocked_since = -1.0;
    }
    return c->ema;
}

int fg_pace_note(FgPace *p, double now, double period_s, double slack_s) {
    if (period_s <= 0.0) return 0;
    if (!p->primed) { p->primed = 1; p->next = now + period_s; return 0; }
    int late = 0;
    if (now > p->next + slack_s) { late = 1; p->next = now; }
    else if (now < p->next - period_s) p->next = now;   /* early: pull in */
    p->next += period_s;
    return late;
}
