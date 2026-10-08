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
        CHECK(role_of(SlotState::None) == Role::Stable);
        CHECK(role_of(SlotState::Valid) == Role::Stable);
    }

    CASE("an image booted by request is dev until the rider promotes it");
    {
        CHECK(role_of(SlotState::PendingVerify) == Role::Dev);
        CHECK(role_of(SlotState::New) == Role::Dev);
    }

    CASE("the other slot: an aborted dev is still a dev");
    {
        CHECK(other_from(true, SlotState::Aborted) == Other::Ready);
        CHECK(other_from(true, SlotState::None) == Other::Ready);
        CHECK(other_from(false, SlotState::None) == Other::Empty);
        CHECK(other_from(true, SlotState::Invalid) == Other::Invalid);
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
