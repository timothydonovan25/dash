#include "core/ota/ota.hpp"

#include <cstring>

namespace core::ota {
namespace {

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// A fixed-width C string field from the image, terminated whether or not
// the image terminated it.
void copy_field(char* dst, size_t cap, const uint8_t* src, size_t len) {
    size_t n = 0;
    while (n < len && n + 1 < cap && src[n] != '\0') {
        dst[n] = static_cast<char>(src[n]);
        ++n;
    }
    dst[n] = '\0';
}

}  // namespace

const char* image_error_name(ImageError e) {
    switch (e) {
        case ImageError::None:         return "ok";
        case ImageError::TooShort:     return "too short";
        case ImageError::BadMagic:     return "not an image";
        case ImageError::WrongChip:    return "wrong chip";
        case ImageError::NoAppDesc:    return "not an app";
        case ImageError::WrongProject: return "not dash";
        case ImageError::TooBig:       return "too big";
    }
    return "?";
}

ImageError check_header(const uint8_t* head, size_t len, size_t file_size,
                        size_t slot_size, ImageInfo& out) {
    out = ImageInfo{};
    if (!head || len < kHeaderBytes || file_size < kHeaderBytes) {
        return ImageError::TooShort;
    }
    if (head[0] != kImageMagic || head[1] == 0) return ImageError::BadMagic;
    if (le16(head + kChipIdOffset) != kChipEsp32P4) return ImageError::WrongChip;
    if (le32(head + kAppDescOffset) != kAppDescMagic) return ImageError::NoAppDesc;

    char project[32];
    copy_field(project, sizeof project, head + kProjectOffset, 32);
    if (std::strcmp(project, kProjectName) != 0) return ImageError::WrongProject;
    if (file_size > slot_size) return ImageError::TooBig;

    copy_field(out.version, sizeof out.version, head + kVersionOffset, kVersionLen);
    return ImageError::None;
}

const char* role_name(Role r) {
    switch (r) {
        case Role::Stable: return "stable";
        case Role::Dev:    return "dev";
    }
    return "?";
}

Role role_of(SlotState running) {
    return running == SlotState::PendingVerify || running == SlotState::New ? Role::Dev
                                                                             : Role::Stable;
}

Other other_from(bool has_valid_image, SlotState state) {
    if (state == SlotState::Invalid) return Other::Invalid;
    return has_valid_image ? Other::Ready : Other::Empty;
}

// --- installing ----------------------------------------------------------

const char* install_error_name(InstallError e) {
    switch (e) {
        case InstallError::None:         return "ok";
        case InstallError::Moving:       return "park first";
        case InstallError::NotStable:    return "run stable first";
        case InstallError::NotDev:       return "run dev first";
        case InstallError::NoCard:       return "no card";
        case InstallError::NoFile:       return "no update/dash.bin";
        case InstallError::BadImage:     return "bad image";
        case InstallError::ReadFailed:   return "read failed";
        case InstallError::FlashFailed:  return "flash write failed";
        case InstallError::VerifyFailed: return "image check failed";
        case InstallError::NoDev:        return "no dev image";
    }
    return "?";
}

bool may_write(float speed_mph, float rpm) {
    return speed_mph < 0.5f && rpm < 1.0f;
}

uint8_t percent(size_t done, size_t total) {
    if (total == 0) return 0;
    if (done >= total) return 100;
    return static_cast<uint8_t>((static_cast<uint64_t>(done) * 100u) / total);
}

}  // namespace core::ota
