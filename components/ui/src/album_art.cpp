#include "ui/album_art.hpp"
#include <cstring>

namespace ui::album_art {

const Cover* find(const char* album) {
    if (!album || !album[0]) return nullptr;
    for (size_t i = 0; i < kGojiraCount; ++i) {
        if (std::strcmp(kGojira[i].album, album) == 0) return &kGojira[i];
    }
    return nullptr;
}

}  // namespace ui::album_art
