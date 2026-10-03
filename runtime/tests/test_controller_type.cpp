#include "psx_controller_type.h"

#include <cstdio>

int main() {
    struct Case { const char* name; bool wheel; } cases[] = {
        {"Logitech G29 Driving Force Racing Wheel", true},
        {"Logitech G920", true},
        {"Thrustmaster T300 RS", true},
        {"Fanatec CSL Elite", true},
        {"MOZA R5", true},
        {"Xbox Wireless Controller", false},
        {"Sony DualSense Wireless Controller", false},
        {"Nintendo Switch Pro Controller", false},
        {nullptr, false},
        {"", false},
    };
    for (const auto& test : cases) {
        if (psx_controller_name_is_wheel(test.name) != test.wheel) {
            std::fprintf(stderr, "classification failed: %s\n",
                         test.name ? test.name : "(null)");
            return 1;
        }
    }
    std::puts("controller type classification: passed");
    return 0;
}
