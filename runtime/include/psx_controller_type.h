#ifndef PSX_CONTROLLER_TYPE_H
#define PSX_CONTROLLER_TYPE_H

#include <algorithm>
#include <cctype>
#include <string>

/* SDL2/3 does not expose a consistently useful wheel class for every mapping.
 * This is a conservative name hint; only mapped controllers with known wheel
 * family names are promoted to the JogCon device protocol. */
inline bool psx_controller_name_is_wheel(const char* name) {
    if (!name || !*name) return false;
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const char* tokens[] = {
        "wheel", "g29", "g920", "g923", "g27", "g25", "driving force",
        "t150", "t300", "t500", "t248", "tx racing", "thrustmaster",
        "fanatec", "moza"
    };
    for (const char* token : tokens)
        if (normalized.find(token) != std::string::npos) return true;
    return false;
}

#endif
