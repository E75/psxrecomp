#include "fntrace.h"
#include <stdio.h>
#include <stdlib.h>

static int effects, image_matches;
void dirty_ram_clear_image_baseline(void) { ++effects; }
void memory_clear_low_boot_scratch(void) { ++effects; }
void cdrom_notify_game_started(void) { ++effects; }
void boot_state_trigger_capture(const CPUState* cpu) { (void)cpu; ++effects; }
int dirty_ram_text_image_matches(uint32_t phys, uint32_t len) {
    return image_matches && phys == 0x6B58Cu && len == 32u;
}
static void check(int ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
int main(void) {
    CPUState cpu = {0};
    fntrace_set_game_range(0x8006B58Cu, 0);
    fntrace_restore_game_started(1);
    fntrace_mark_game_started(&cpu);
    check(fntrace_is_game_started() && !effects, "cold game restore skips destructive handoff");
    fntrace_restore_game_started(0);
    check(!fntrace_is_game_started() && !effects, "pre-game restore resets warm latch");
    fntrace_mark_game_started(&cpu);
    check(effects == 4, "normal boot retains all handoff effects");
    fntrace_mark_game_started(&cpu);
    check(effects == 4, "handoff remains one-shot");
    image_matches = 1;
    fntrace_set_game_range(0x8006B58Cu, 0);
    fntrace_restore_game_started(-1);
    check(fntrace_is_game_started() && effects == 4, "legacy game restore infers loaded entry");
    image_matches = 0;
    fntrace_restore_game_started(-1);
    check(!fntrace_is_game_started(), "legacy BIOS restore does not inherit warm game latch");
    puts("restored handoff checks passed");
    return 0;
}
