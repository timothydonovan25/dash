// Stable and dev firmware, and updating them (§11.7).
//
// The two app slots hold a stable image and a dev image. Which is which is
// not stored anywhere: it is read off the bootloader's own state for the
// running slot, because that is the one record a crash cannot leave out of
// date. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE:
//
//   - A USB flash erases otadata, so its image boots with no state at all:
//     Undefined. A confirmed image is Valid. Either one running is stable.
//   - esp_ota_set_boot_partition() marks the slot New. The bootloader turns
//     New into PendingVerify as it boots it, and a slot still PendingVerify
//     at the boot after that into Aborted, then boots the other slot.
//
// So a dev image that never confirms itself runs exactly once per request,
// and any reset, crash or power cut lands back on stable. Promoting dev is
// the same boot plus a marker in NVS that tells the new image it is allowed
// to confirm itself, once it has shown it can run the dash.
//
// Everything here is the decisions over that, with no ESP header, so they are
// a unit test rather than a bricked dash. `ota_service` does the flash calls.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::ota {

// --- image check ---------------------------------------------------------
//
// An app image starts with esp_image_header_t (24 bytes), then the first
// segment's header (8 bytes), then that segment's data, which begins with
// esp_app_desc_t (256 bytes). The offsets are IDF's, checked against a real
// dash.bin; they are spelled out here as bytes so the check builds on the
// host. A full verify (hash, every segment) is esp_ota_end()'s job — this
// only refuses the wrong file before a byte of the slot is erased.

inline constexpr uint8_t  kImageMagic   = 0xE9;
inline constexpr uint16_t kChipEsp32P4  = 0x0012;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432;
inline constexpr char     kProjectName[] = "dash";

inline constexpr size_t kChipIdOffset   = 12;
inline constexpr size_t kAppDescOffset  = 32;
inline constexpr size_t kVersionOffset  = kAppDescOffset + 16;
inline constexpr size_t kProjectOffset  = kAppDescOffset + 48;
inline constexpr size_t kVersionLen     = 32;
// How much of the file check_header() needs to see.
inline constexpr size_t kHeaderBytes    = kAppDescOffset + 256;

enum class ImageError : uint8_t {
    None = 0,
    TooShort,       // fewer than kHeaderBytes, or a file too small to be an app
    BadMagic,       // not an ESP app image at all
    WrongChip,      // built for another chip
    NoAppDesc,      // an image, but not an application
    WrongProject,   // an application, but not this dash's
    TooBig,         // will not fit the slot
};
const char* image_error_name(ImageError e);

struct ImageInfo {
    char version[kVersionLen] = {};
};

// `head` is the first `len` bytes of a file `file_size` long, going into a
// slot of `slot_size`.
ImageError check_header(const uint8_t* head, size_t len, size_t file_size,
                        size_t slot_size, ImageInfo& out);

// --- roles ---------------------------------------------------------------

// esp_ota_img_states_t without the header. `None` is "otadata has no entry
// for this slot", which is what a USB flash leaves.
enum class SlotState : uint8_t { None = 0, New, PendingVerify, Valid, Invalid, Aborted };

enum class Role : uint8_t {
    Stable = 0,
    Dev,          // booted once from stable; never confirms, so a reset ends it
    Confirming,   // a promoted image proving itself before it confirms
};
const char* role_name(Role r);

struct Boot {
    Role role = Role::Stable;
    // The NVS promote marker is stale and should be deleted.
    bool clear_marker = false;
    // Stable is running, but the marker says a promotion was underway: the
    // promoted image did not survive to confirm itself.
    bool promotion_failed = false;
};

// `marker_slot` is the slot index the promote marker names, or -1 for none.
Boot role_of(SlotState running, int running_slot, int marker_slot);

// Whether a Confirming image has shown it can run the dash: long enough up,
// the render loop drawing, and `ctrl` publishing.
inline constexpr uint32_t kConfirmAfterMs = 20000;
bool should_confirm(uint32_t uptime_ms, uint16_t render_hz_x10, bool ctrl_alive);

// What the slot that is not running holds, for the screen and to decide
// whether "boot dev once" is offered. An Aborted dev is normal — every dev
// run ends that way — so it is still Ready.
enum class Other : uint8_t { Unknown = 0, Empty, Ready, Invalid };
Other other_from(bool has_valid_image, SlotState state);

// --- the boot window -----------------------------------------------------
//
// Stable shows, for kBannerMs after a power-on, that it is starting stable
// and that any button opens a chooser. No input and it carries on. The
// chooser has two rows, Stable and Dev; it falls back to Stable after
// kChooserMs without a press. Dev can be picked only if there is a dev.

inline constexpr uint32_t kBannerMs  = 3000;
inline constexpr uint32_t kChooserMs = 10000;

enum class Phase : uint8_t { Off = 0, Banner, Chooser, Done };
enum class Choice : uint8_t { None = 0, Stable, Dev };
// The pod's buttons, as core::nav numbers them; repeated here so this header
// does not drag nav in.
enum class Key : uint8_t { A = 0, B, C, D, E };

class BootWindow {
public:
    void begin(uint32_t now_ms, bool dev_available);
    // Returns true if the window consumed the press.
    bool press(Key k, uint32_t now_ms);
    void tick(uint32_t now_ms);

    Phase    phase() const { return phase_; }
    bool     active() const { return phase_ == Phase::Banner || phase_ == Phase::Chooser; }
    uint8_t  focus() const { return focus_; }      // 0 Stable, 1 Dev
    bool     dev_available() const { return dev_; }
    uint32_t left_ms(uint32_t now_ms) const;
    // The choice, once. Dev means the caller posts the boot-dev command.
    Choice   take_choice();

private:
    Phase    phase_ = Phase::Off;
    Choice   choice_ = Choice::None;
    bool     dev_ = false;
    uint8_t  focus_ = 0;
    uint32_t since_ms_ = 0;
};

// --- installing ----------------------------------------------------------

enum class Install : uint8_t { Idle = 0, Checking, Writing, Done, Failed };

enum class InstallError : uint8_t {
    None = 0,
    Moving,        // refused: not parked
    NotStable,     // refused: only stable writes the dev slot
    NoCard,
    NoFile,
    BadImage,      // check_header said no; see the ImageError
    ReadFailed,    // card pulled, or a read error, mid-file
    FlashFailed,   // esp_ota_begin/write
    VerifyFailed,  // esp_ota_end: the image's own hash disagrees
    NoDev,         // boot/promote with nothing in the dev slot
};
const char* install_error_name(InstallError e);

// Writing flash stops the cache on both cores for an erase, long enough to
// blow the 3 ms tach-to-lamp budget. So only parked: not moving, engine off.
bool may_write(float speed_mph, float rpm);

// Whole percent, 0..100, safe for total 0.
uint8_t percent(size_t done, size_t total);

}  // namespace core::ota
