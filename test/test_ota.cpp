#include "check.hpp"
#include "core/ota/ota.hpp"

#include <cstring>
#include <vector>

using namespace core::ota;

namespace {

// The first kHeaderBytes of an app image, laid out as IDF lays them out:
// esp_image_header_t, the first segment header, then esp_app_desc_t.
std::vector<uint8_t> image(uint16_t chip = kChipEsp32P4,
                           const char* project = "dash",
                           const char* version = "v1.2.3") {
    std::vector<uint8_t> b(kHeaderBytes, 0);
    b[0] = kImageMagic;
    b[1] = 7;                                         // segment count
    b[kChipIdOffset]     = static_cast<uint8_t>(chip & 0xFF);
    b[kChipIdOffset + 1] = static_cast<uint8_t>(chip >> 8);
    const uint32_t m = kAppDescMagic;
    for (int i = 0; i < 4; ++i) b[kAppDescOffset + i] = static_cast<uint8_t>(m >> (8 * i));
    std::memcpy(&b[kVersionOffset], version, std::strlen(version));
    std::memcpy(&b[kProjectOffset], project, std::strlen(project));
    return b;
}

constexpr size_t kSlot = 6u * 1024 * 1024;
constexpr size_t kFile = 1200000;

ImageError verify(const std::vector<uint8_t>& b, size_t file = kFile,
                 ImageInfo* out = nullptr) {
    ImageInfo info;
    const ImageError e = check_header(b.data(), b.size(), file, kSlot, info);
    if (out) *out = info;
    return e;
}

}  // namespace

int main() {
    CASE("a dash image for this chip passes, and its version is read");
    {
        ImageInfo info;
        CHECK(verify(image(), kFile, &info) == ImageError::None);
        CHECK(std::strcmp(info.version, "v1.2.3") == 0);
    }

    CASE("a full-width version string is cut, not overrun");
    {
        const char* v = "0123456789012345678901234567890123456789";   // 40
        std::vector<uint8_t> b = image(kChipEsp32P4, "dash", "");
        std::memcpy(&b[kVersionOffset], v, kVersionLen);               // no NUL
        ImageInfo info;
        CHECK(verify(b, kFile, &info) == ImageError::None);
        CHECK(std::strlen(info.version) == kVersionLen - 1);
    }

    CASE("each wrong file is refused with its own reason");
    {
        std::vector<uint8_t> b = image();
        b[0] = 0x00;
        CHECK(verify(b) == ImageError::BadMagic);

        b = image();
        b[1] = 0;                                     // no segments
        CHECK(verify(b) == ImageError::BadMagic);

        // The C6's network_adapter.bin is the likeliest wrong file on the card.
        CHECK(verify(image(0x000D)) == ImageError::WrongChip);

        b = image();
        b[kAppDescOffset] ^= 0xFF;                    // a bootloader, say
        CHECK(verify(b) == ImageError::NoAppDesc);

        CHECK(verify(image(kChipEsp32P4, "panel_test")) == ImageError::WrongProject);
        CHECK(verify(image(kChipEsp32P4, "dashboard")) == ImageError::WrongProject);

        CHECK(verify(image(), kSlot + 1) == ImageError::TooBig);
        CHECK(verify(image(), kSlot) == ImageError::None);
    }

    CASE("a short read or a tiny file is refused before anything is erased");
    {
        std::vector<uint8_t> b = image();
        ImageInfo info;
        CHECK(check_header(b.data(), kHeaderBytes - 1, kFile, kSlot, info) ==
              ImageError::TooShort);
        CHECK(verify(b, kHeaderBytes - 1) == ImageError::TooShort);
        CHECK(check_header(nullptr, 0, kFile, kSlot, info) == ImageError::TooShort);
    }

    CASE("a USB-flashed or confirmed image is stable");
    {
        CHECK(role_of(SlotState::None, 0, -1).role == Role::Stable);
        CHECK(role_of(SlotState::Valid, 1, -1).role == Role::Stable);
        const Boot b = role_of(SlotState::Valid, 0, -1);
        CHECK(!b.clear_marker);
        CHECK(!b.promotion_failed);
    }

    CASE("an image booted by request with no marker is dev");
    {
        CHECK(role_of(SlotState::PendingVerify, 1, -1).role == Role::Dev);
        CHECK(role_of(SlotState::New, 1, -1).role == Role::Dev);
        // A marker for the other slot does not promote this one.
        const Boot b = role_of(SlotState::PendingVerify, 1, 0);
        CHECK(b.role == Role::Dev);
        CHECK(!b.clear_marker);
    }

    CASE("a promoted image is on probation until it confirms");
    {
        const Boot b = role_of(SlotState::PendingVerify, 1, 1);
        CHECK(b.role == Role::Confirming);
        CHECK(!b.clear_marker);           // cleared only once it confirms
    }

    CASE("stable with a marker for the other slot: the promotion failed");
    {
        const Boot b = role_of(SlotState::Valid, 0, 1);
        CHECK(b.role == Role::Stable);
        CHECK(b.clear_marker);
        CHECK(b.promotion_failed);
    }

    CASE("stable with a marker for itself: confirmed, then lost power");
    {
        const Boot b = role_of(SlotState::Valid, 1, 1);
        CHECK(b.role == Role::Stable);
        CHECK(b.clear_marker);
        CHECK(!b.promotion_failed);
    }

    CASE("a promoted image confirms only once it is running the dash");
    {
        CHECK(!should_confirm(kConfirmAfterMs - 1, 600, true));
        CHECK(!should_confirm(kConfirmAfterMs, 0, true));       // no frames
        CHECK(!should_confirm(kConfirmAfterMs, 600, false));    // ctrl stalled
        CHECK(should_confirm(kConfirmAfterMs, 600, true));
    }

    CASE("the other slot: an aborted dev is still a dev");
    {
        CHECK(other_from(true, SlotState::Aborted) == Other::Ready);
        CHECK(other_from(true, SlotState::None) == Other::Ready);
        CHECK(other_from(false, SlotState::None) == Other::Empty);
        CHECK(other_from(true, SlotState::Invalid) == Other::Invalid);
    }

    CASE("no input: the banner goes and stable carries on");
    {
        BootWindow w;
        CHECK(w.phase() == Phase::Off);
        CHECK(!w.active());
        w.begin(500, true);
        CHECK(w.active());
        CHECK(w.left_ms(500) == kBannerMs);
        w.tick(500 + kBannerMs - 1);
        CHECK(w.phase() == Phase::Banner);
        w.tick(500 + kBannerMs);
        CHECK(w.phase() == Phase::Done);
        CHECK(!w.active());
        CHECK(w.take_choice() == Choice::Stable);
        CHECK(w.take_choice() == Choice::None);   // once
    }

    CASE("any button opens the chooser, which starts on stable");
    {
        BootWindow w;
        w.begin(0, true);
        CHECK(w.press(Key::A, 1000));
        CHECK(w.phase() == Phase::Chooser);
        CHECK(w.focus() == 0);
        CHECK(w.left_ms(1000) == kChooserMs);
    }

    CASE("picking dev");
    {
        BootWindow w;
        w.begin(0, true);
        w.press(Key::E, 100);       // opens, does not pick
        CHECK(w.phase() == Phase::Chooser);
        w.press(Key::D, 200);
        CHECK(w.focus() == 1);
        w.press(Key::E, 300);
        CHECK(w.phase() == Phase::Done);
        CHECK(w.take_choice() == Choice::Dev);
    }

    CASE("C and D both move between the two rows");
    {
        BootWindow w;
        w.begin(0, true);
        w.press(Key::B, 0);
        w.press(Key::C, 0);
        CHECK(w.focus() == 1);
        w.press(Key::C, 0);
        CHECK(w.focus() == 0);
    }

    CASE("dev cannot be picked when there is none");
    {
        BootWindow w;
        w.begin(0, false);
        w.press(Key::A, 0);
        w.press(Key::D, 0);
        CHECK(w.focus() == 0);
        w.press(Key::E, 0);
        CHECK(w.take_choice() == Choice::Stable);
    }

    CASE("an abandoned chooser falls back to stable, and presses keep it open");
    {
        BootWindow w;
        w.begin(0, true);
        w.press(Key::A, 1000);
        w.press(Key::D, 1000 + kChooserMs - 1);              // restarts the clock
        w.tick(1000 + kChooserMs);
        CHECK(w.phase() == Phase::Chooser);
        w.tick(1000 + 2 * kChooserMs - 1);
        CHECK(w.phase() == Phase::Done);
        CHECK(w.take_choice() == Choice::Stable);
    }

    CASE("once done, the window lets presses through to the dash");
    {
        BootWindow w;
        CHECK(!w.press(Key::A, 0));           // never opened
        w.begin(0, true);
        w.tick(kBannerMs);
        CHECK(!w.press(Key::A, kBannerMs + 1));
    }

    CASE("only parked: not moving, engine off");
    {
        CHECK(may_write(0.0f, 0.0f));
        CHECK(!may_write(3.0f, 0.0f));
        CHECK(!may_write(0.0f, 1400.0f));
    }

    CASE("percent is whole, bounded, and safe for an empty total");
    {
        CHECK(percent(0, 0) == 0);
        CHECK(percent(0, 1000) == 0);
        CHECK(percent(999, 1000) == 99);
        CHECK(percent(1000, 1000) == 100);
        CHECK(percent(5000, 1000) == 100);
        CHECK(percent(3000000, 6000000) == 50);
    }

    return check::report("ota");
}
