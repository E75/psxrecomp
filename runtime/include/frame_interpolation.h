#ifndef PSX_FRAME_INTERPOLATION_H
#define PSX_FRAME_INTERPOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameInterpolationSchedule {
    double source_deadline;
    double next_present_deadline;
    double frame_start;
    double frame_end;
    double target_period;
    /* Blend-phase window of this source interval (begin_phase). begin()
     * plans the whole 0..1 window, which is the historical behaviour. */
    double phase_lo;
    double phase_hi;
} FrameInterpolationSchedule;

/* Plan one stock guest-frame interval. Presentation deadlines remain anchored
 * across calls so guest work consumes part of the interval instead of slowing
 * the simulation. A host that falls behind catches up (intervals run without
 * waiting) until it is FRAME_INTERP_CATCHUP_MAX_PERIODS source periods late,
 * the same window as the stock frame pacer (frame_pacing.c); only then is the
 * debt forgiven and the anchor reset. Returns zero when the rates or host
 * clock are invalid. */
#define FRAME_INTERP_CATCHUP_MAX_PERIODS 12.0
int frame_interpolation_schedule_begin(FrameInterpolationSchedule *schedule,
                                       uint64_t now, uint64_t frequency,
                                       double source_hz, double target_hz);

/* Return each presentation deadline in the current source interval. Alpha is
 * the temporal blend position from the previous completed frame to the current
 * one. Deadlines missed by more than one output period are coalesced. */
int frame_interpolation_schedule_next(FrameInterpolationSchedule *schedule,
                                      uint64_t now, uint64_t *deadline,
                                      float *alpha);

/* Same as begin(), but the returned blend alphas cover only [phase_lo,
 * phase_hi] of the frame-to-frame transition (both clamped to 0..1). A game
 * that flips every P VBlanks presents VBlank k (0-based since the flip) with
 * the window [k/P, (k+1)/P], so one crossfade spans the whole game frame
 * instead of finishing after the first VBlank and holding for the rest.
 * begin() is begin_phase(0, 1). */
int frame_interpolation_schedule_begin_phase(FrameInterpolationSchedule *schedule,
                                             uint64_t now, uint64_t frequency,
                                             double source_hz, double target_hz,
                                             double phase_lo, double phase_hi);

/* Render passes present between guest VBlanks. The next output deadline, if
 * it is already due (<= now) and not later than `horizon` (host ticks), with
 * stale deadlines coalesced to the newest one as begin() does. It is not
 * consumed: call frame_interpolation_schedule_consume() once it is shown, so
 * the next source interval simply continues after it. */
int frame_interpolation_schedule_due(FrameInterpolationSchedule *schedule,
                                     uint64_t now, double horizon,
                                     uint64_t *deadline);
void frame_interpolation_schedule_consume(FrameInterpolationSchedule *schedule);

uint64_t frame_interpolation_schedule_end(
    const FrameInterpolationSchedule *schedule);
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *schedule);

/* Flip-aware blend source (psx_mod_set_frame_interpolation_source FLIP).
 * Pure bookkeeping, one call per presented guest VBlank: new_frame says the
 * displayed image changed (display origin moved, or the displayed rect was
 * redrawn). Tracks P, the last observed flip period in VBlanks (clamped
 * 1..FRAME_FLIP_PERIOD_MAX; 1 until two new frames were seen), and k, the
 * VBlanks since the last new frame, and yields this VBlank's blend window
 * [k/P, (k+1)/P] clamped to 0..1. A frame that arrives late holds at 1; one
 * that arrives early restarts at 0 from the frame that was fully shown. */
#define FRAME_FLIP_PERIOD_MAX 4u
typedef struct FrameFlipTracker {
    uint32_t since_flip;  /* VBlanks since the last new frame, before this one */
    uint32_t period;      /* P */
    uint32_t frames;      /* new frames seen (saturating) */
} FrameFlipTracker;

/* Monotonic game-time stamp for a composed presentation. source_frame counts
 * newly captured game frames; phase_q16 is 0..65536 within the transition to
 * the next one. A delayed render-pass generation must not replace a newer
 * frame already shown on screen. */
typedef struct FrameInterpolationPresentTime {
    uint64_t source_frame;
    uint32_t phase_q16;
    int valid;
} FrameInterpolationPresentTime;

int frame_interpolation_present_time_is_monotonic(
    const FrameInterpolationPresentTime *last, uint64_t source_frame,
    uint32_t phase_q16);
void frame_interpolation_present_time_record(
    FrameInterpolationPresentTime *last, uint64_t source_frame,
    uint32_t phase_q16);
void frame_interpolation_present_time_reset(FrameInterpolationPresentTime *last);
void frame_interpolation_present_history_time(uint64_t captures, int valid,
                                               float alpha, uint64_t *source,
                                               uint32_t *phase);
uint32_t frame_interpolation_present_pass_phase(const uint32_t *phases,
                                                 uint32_t lo, uint32_t hi,
                                                 float weight);
/* The same admission/emission seam used by the GL presenter. An image is
 * recorded only after its swap callback succeeds. */
int frame_interpolation_present_emit(FrameInterpolationPresentTime *last,
                                     uint64_t source, uint32_t phase,
                                     int (*swap)(void *), void *user);

void frame_flip_tracker_reset(FrameFlipTracker *tracker);
/* Whether a presented VBlank shows a new guest frame (the tracker's
 * new_frame): the presented geometry changed, no frame is held yet, the
 * displayed rect was redrawn since the last present, or the display origin
 * moved from (last_x, last_y) to (origin_x, origin_y), i.e. a flip. Anything
 * else re-presents the image already held and is a duplicate. */
int frame_flip_is_new_frame(int geometry_changed, int history_empty,
                            int redrawn, int origin_x, int origin_y,
                            int last_x, int last_y);
/* Returns k for this VBlank. */
uint32_t frame_flip_tracker_vblank(FrameFlipTracker *tracker, int new_frame,
                                   double *phase_lo, double *phase_hi);

#ifdef __cplusplus
}
#endif

#endif
