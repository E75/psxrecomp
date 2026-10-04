#include <assert.h>
#include "psx_trigger.h"

int main(void) {
    assert(psx_trigger_axis_to_u8(-32768) == 0);
    assert(psx_trigger_axis_to_u8(0) == 0);
    assert(psx_trigger_axis_to_u8(8192) == 64);
    assert(psx_trigger_axis_to_u8(16384) == 128);
    assert(psx_trigger_axis_to_u8(24575) == 191);
    assert(psx_trigger_axis_to_u8(32767) == 255);
    return 0;
}
