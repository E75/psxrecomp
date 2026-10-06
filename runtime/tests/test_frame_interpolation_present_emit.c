#include <stdint.h>
#include <stdio.h>
#include "frame_interpolation.h"

static FrameInterpolationPresentTime cursor;
static uint64_t emitted_source;
static uint32_t emitted_phase;
static int swaps;
static int fail_swap;

static int mock_gl_swap(void *unused) {
    (void)unused;
    if (fail_swap) return 0;
    swaps++;
    return 1;
}

static int present(uint64_t source, uint32_t phase, int expected) {
    int before = swaps;
    int result = frame_interpolation_present_emit(&cursor, source, phase,
                                                   mock_gl_swap, NULL);
    if (result != expected || swaps != before + (expected == 1)) return 0;
    if (result == 1) {
        if (emitted_source > source ||
            (emitted_source == source && emitted_phase > phase)) return 0;
        emitted_source = source;
        emitted_phase = phase;
    }
    return 1;
}

int main(void) {
    uint64_t source = 0;
    uint32_t phase = 0;
    const uint32_t pass_phases[] = {0, 16384, 49152, 65536};

    /* These selectors and the emitter are the production GL present seam. */
    frame_interpolation_present_history_time(40, 2, 1.f, &source, &phase);
    if (source != 39 || phase != 65536 || !present(source, phase, 1)) return 1;
    phase = frame_interpolation_present_pass_phase(pass_phases, 1, 2, 0.5f);
    if (phase != 32768 || !present(40, phase, 1)) return 2;
    frame_interpolation_present_history_time(41, 2, 0.f, &source, &phase);
    if (source != 40 || phase != 0 || !present(source, phase, -1)) return 3;
    if (!present(39, 65536, -1)) return 4;
    if (!present(40, 49152, 1)) return 5;
    fail_swap = 1;
    if (!present(41, 0, 0) || cursor.source_frame != 40) return 6;
    fail_swap = 0;
    if (!present(41, 0, 1)) return 7;
    frame_interpolation_present_time_reset(&cursor);
    emitted_source = emitted_phase = 0;
    if (!present(1, 0, 1)) return 8;
    printf("GL presentation selection/emission monotonic: %d swaps\n", swaps);
    return 0;
}
