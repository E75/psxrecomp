#include <stdint.h>
#include <stdio.h>

#include "frame_interpolation.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { puts("FAIL: " message); failures++; } \
} while (0)

int main(void) {
    FrameInterpolationPresentTime last = {0};

    CHECK(frame_interpolation_present_time_is_monotonic(&last, 40, 0),
          "the first source frame is eligible");
    frame_interpolation_present_time_record(&last, 40, 0);
    CHECK(frame_interpolation_present_time_is_monotonic(&last, 40, 16384),
          "in-between images advance within a source frame");
    frame_interpolation_present_time_record(&last, 40, 49152);
    CHECK(frame_interpolation_present_time_is_monotonic(&last, 41, 0),
          "the next game's frame follows the previous generation");
    frame_interpolation_present_time_record(&last, 41, 0);

    CHECK(!frame_interpolation_present_time_is_monotonic(&last, 40, 65536),
          "a late previous generation cannot replace the next game frame");
    frame_interpolation_present_time_record(&last, 41, 32768);
    CHECK(!frame_interpolation_present_time_is_monotonic(&last, 41, 0),
          "an earlier phase in the current generation cannot be replayed");
    CHECK(frame_interpolation_present_time_is_monotonic(&last, 41, 32768),
          "a later image in the current generation remains eligible");

    CHECK(frame_interpolation_present_time_is_monotonic(&last, 41, 32768),
          "the same timestamp may be presented again");
    CHECK(frame_interpolation_present_time_is_monotonic(&last, 42, 0),
          "a frame boundary advances after any in-between phase");

    frame_interpolation_present_time_reset(&last);
    CHECK(frame_interpolation_present_time_is_monotonic(&last, 1, 0),
          "a history reset starts a new presentation sequence");

    if (failures) return 1;
    puts("frame interpolation present-time monotonicity passed");
    return 0;
}
