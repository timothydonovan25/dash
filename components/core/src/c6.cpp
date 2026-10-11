#include "core/c6/c6.hpp"

namespace core::c6 {

int32_t build_id(const uint8_t sha[4]) {
    const uint32_t v = (static_cast<uint32_t>(sha[0]) << 24) | (static_cast<uint32_t>(sha[1]) << 16) |
                       (static_cast<uint32_t>(sha[2]) << 8) | static_cast<uint32_t>(sha[3]);
    const int32_t id = static_cast<int32_t>(v & 0x7FFFFFFFu);
    return id == 0 ? 1 : id;
}

bool parse_version(const char* s, FwId& out) {
    if (!s) return false;
    uint32_t part[3] = {0, 0, 0};
    int i = 0;
    bool digit = false;
    for (const char* p = s; ; ++p) {
        const char c = *p;
        if (c >= '0' && c <= '9') {
            if (part[i] > 100000) return false;
            part[i] = part[i] * 10 + static_cast<uint32_t>(c - '0');
            digit = true;
        } else if ((c == '.' || c == '\0') && digit) {
            if (c == '\0') break;
            if (++i > 2) return false;
            digit = false;
        } else {
            return false;
        }
    }
    if (i != 2) return false;
    out.major = part[0];
    out.minor = part[1];
    out.patch = part[2];
    return true;
}

const char* action_name(Action a) {
    switch (a) {
        case Action::None:         return "current";
        case Action::Push:         return "push";
        case Action::AlreadyTried: return "already tried";
        case Action::DevRefused:   return "dev refused";
    }
    return "?";
}

Action decide(const FwId& running, const FwId& image, int32_t tried_build, bool running_dev) {
    if (!image.valid || image.build == 0) return Action::None;   // nothing to offer
    // A C6 that did not answer is not evidence of anything: leave it.
    if (!running.valid) return Action::None;
    const bool same = running.build == image.build && running.major == image.major &&
                      running.minor == image.minor && running.patch == image.patch;
    if (same) return Action::None;
    if (tried_build == image.build) return Action::AlreadyTried;
    if (running_dev && running.major != image.major) return Action::DevRefused;
    return Action::Push;
}

}  // namespace core::c6
