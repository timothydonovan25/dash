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
        case Role::Stable:     return "stable";
        case Role::Dev:        return "dev";
        case Role::Confirming: return "confirming";
    }
    return "?";
}

Boot role_of(SlotState running, int running_slot, int marker_slot) {
    Boot b;
    const bool marked = marker_slot >= 0;
    if (running == SlotState::PendingVerify || running == SlotState::New) {
        // Booted by request rather than by default. A marker naming this
        // slot makes it a promotion on probation; anything else is dev. A
        // marker naming the other slot cannot happen from the dash itself,
        // and is left alone for stable to clear.
        b.role = (marked && marker_slot == running_slot) ? Role::Confirming : Role::Dev;
        return b;
    }
    // Valid, or no state at all after a USB flash: stable. A marker left
    // over means either the promoted image confirmed and lost power before
    // deleting it (it names this slot), or it never confirmed and the
    // bootloader came back here (it names the other one).
    b.role = Role::Stable;
    if (marked) {
        b.clear_marker = true;
        b.promotion_failed = marker_slot != running_slot;
    }
    return b;
}

bool should_confirm(uint32_t uptime_ms, uint16_t render_hz_x10, bool ctrl_alive) {
    return uptime_ms >= kConfirmAfterMs && render_hz_x10 > 0 && ctrl_alive;
}

Other other_from(bool has_valid_image, SlotState state) {
    if (state == SlotState::Invalid) return Other::Invalid;
    return has_valid_image ? Other::Ready : Other::Empty;
}

// --- BootWindow ----------------------------------------------------------

void BootWindow::begin(uint32_t now_ms, bool dev_available) {
    phase_ = Phase::Banner;
    choice_ = Choice::None;
    dev_ = dev_available;
    focus_ = 0;
    since_ms_ = now_ms;
}

bool BootWindow::press(Key k, uint32_t now_ms) {
    if (phase_ == Phase::Banner) {
        // Any button: the rider is asking for the menu, whichever it was.
        phase_ = Phase::Chooser;
        since_ms_ = now_ms;
        return true;
    }
    if (phase_ != Phase::Chooser) return false;

    since_ms_ = now_ms;   // every press restarts the chooser's timeout
    switch (k) {
        case Key::C:
        case Key::D:
            // Two rows: either key moves to the other one, if it can.
            if (dev_) focus_ = static_cast<uint8_t>(focus_ ^ 1u);
            break;
        case Key::E:
            choice_ = (focus_ == 1 && dev_) ? Choice::Dev : Choice::Stable;
            phase_ = Phase::Done;
            break;
        default:
            break;      // A and B do nothing here, but are still swallowed
    }
    return true;
}

void BootWindow::tick(uint32_t now_ms) {
    if (phase_ == Phase::Banner && now_ms - since_ms_ >= kBannerMs) {
        phase_ = Phase::Done;
        choice_ = Choice::Stable;
    } else if (phase_ == Phase::Chooser && now_ms - since_ms_ >= kChooserMs) {
        phase_ = Phase::Done;
        choice_ = Choice::Stable;
    }
}

uint32_t BootWindow::left_ms(uint32_t now_ms) const {
    const uint32_t span = phase_ == Phase::Banner  ? kBannerMs
                        : phase_ == Phase::Chooser ? kChooserMs
                                                   : 0;
    const uint32_t gone = now_ms - since_ms_;
    return gone >= span ? 0 : span - gone;
}

Choice BootWindow::take_choice() {
    const Choice c = choice_;
    choice_ = Choice::None;
    return c;
}

// --- installing ----------------------------------------------------------

const char* install_error_name(InstallError e) {
    switch (e) {
        case InstallError::None:         return "ok";
        case InstallError::Moving:       return "park first";
        case InstallError::NotStable:    return "run stable first";
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
