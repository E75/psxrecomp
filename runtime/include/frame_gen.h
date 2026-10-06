#ifndef PSX_FRAME_GEN_H
#define PSX_FRAME_GEN_H
/*
 * Frame generation from recorded draw lists ([video] frame_generation,
 * docs/FRAME_GENERATION.md). The renderer-independent half: the primitives
 * of two consecutive game frames (one per display flip), their matching,
 * vertex interpolation, how many in-between frames fit, and the breaker.
 * The GL backend (gpu_gl_renderer.c) owns the record lists themselves,
 * drawing and presenting; nothing here touches GL or guest state.
 *
 * A primitive is a triangle of one frame's draw list with a match key (the
 * draw op, texture page, CLUT, texture coordinates, the draw area it was
 * clipped to) and its three screen positions in native pixels relative to
 * that frame's displayed buffer. Matching pairs each primitive of the newer
 * frame with one of the older frame's, in draw order: the same key, every
 * vertex moved at most max_move, and the three vertices moved alike (a
 * rigid-ish motion, at most max_deform apart). A primitive with no partner
 * is drawn where the newer frame has it.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgPrim {
    uint32_t rec;        /* index of its record in the owner's list */
    uint32_t key;        /* match key (fg_hash) */
    float    x[3], y[3]; /* native px, relative to the list's display origin */
} FgPrim;

typedef struct FgPrimList {
    FgPrim  *v;
    uint32_t n, cap;
} FgPrimList;

void fg_prims_reset(FgPrimList *l);
void fg_prims_free(FgPrimList *l);
/* Append; returns 0 when out of memory. */
int  fg_prims_add(FgPrimList *l, const FgPrim *p);

/* FNV-1a over 32-bit words, for building keys. */
uint32_t fg_hash(uint32_t h, const int32_t *w, int n);
#define FG_HASH_INIT 2166136261u

typedef struct FgMatchParams {
    float max_move;      /* native px a vertex may move between the frames */
    float max_deform;    /* native px the vertices' moves may differ by */
    int   window;        /* same-key candidates considered per primitive */
} FgMatchParams;
void fg_match_defaults(FgMatchParams *p);

typedef struct FgMatchStats {
    uint32_t prims, matched, moved, unmatched;
} FgMatchStats;

/* For every primitive of `newer`, the index of its partner in `older` or -1
 * (match[newer->n]). Each older primitive pairs at most once. Returns the
 * matched count. */
uint32_t fg_match(const FgPrimList *older, const FgPrimList *newer,
                  const FgMatchParams *p, int32_t *match, FgMatchStats *st);

/* Positions at phase t between a (t = 0) and b (t = 1). */
void fg_lerp(const FgPrim *a, const FgPrim *b, double t, float x[3], float y[3]);

/* How many in-between frames to draw per game frame.
 *   flip_s       the game frame's interval (time between flips)
 *   refresh_hz   the display's refresh rate
 *   real_cost_s  the render thread's cost of one game frame (all its VBlank
 *                frames), 0 when unknown
 *   gen_cost_s   the cost of one generated frame, 0 when not yet measured
 *   budget       share of the interval the real and generated work may use
 * Returns 0..slots-1, where slots = round(flip_s * refresh_hz); an unknown
 * generation cost allows one frame while the real cost leaves half the
 * interval, so the cost gets measured. */
int fg_plan(double flip_s, double refresh_hz, double real_cost_s,
            double gen_cost_s, double budget, int max_gens);

/* Breaker: after a trip, generation stays off for hold seconds; a trip
 * within `repeat_s` of the end of the last hold doubles the hold, up to max. */
typedef struct FgBreaker {
    double until, hold, last_end;
    double base_hold, max_hold, repeat_s;
    uint32_t trips;
    const char *reason;
} FgBreaker;
void fg_breaker_init(FgBreaker *b, double base_hold, double max_hold, double repeat_s);
void fg_breaker_trip(FgBreaker *b, double now, const char *reason);
int  fg_breaker_open(const FgBreaker *b, double now);   /* 1 = generation allowed */

#ifdef __cplusplus
}
#endif
#endif
