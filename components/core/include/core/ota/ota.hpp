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
// and any reset, crash or power cut lands back on stable. Promotion is the
// rider's call, never a timer: while dev is running, DIAG -> Update ->
// Promote confirms it, and from then on it is stable.
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
inline constexpr size_t kDateOffset     = kAppDescOffset + 96;   // "Oct  8 2026"
inline constexpr size_t kDateLen        = 16;
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
    WrongBoard,     // built for the other board: demo on the bike, or back
};
const char* image_error_name(ImageError e);

struct ImageInfo {
    char version[kVersionLen] = {};
    char date[11] = {};             // "2026-10-08", or empty if it has none
};

// --- what a version says --------------------------------------------------
//
// firmware.txt makes every version "<number> <board> <feature>", e.g.
// "1.0 demo Wi-Fi OTA" (see cmake/dash_version.cmake). Images from before
// that say something else, and are taken as they are.

// IDF's __DATE__ ("Oct  8 2026") as "2026-10-08". Empty if it is not one.
void iso_date(const char* idf_date, char* out, size_t n);

// The board word of a version, into `out`; false if the version is not in
// the "<number> <board> <feature>" form.
bool board_of(const char* version, char* out, size_t n);

// Refuses an image for the other board. Either version without a board --
// an older image, or a hand-built one -- is not refused: there is nothing to
// compare, and the dev slot's fall-back is the safety net either way.
ImageError check_board(const char* running_version, const char* image_version);

// The file name for an image, as cmake/dash_name_bin.cmake names a build:
// "1.0 demo Wi-Fi OTA" built "2026-10-08" is
// "dash-1.0-2026-10-08-demo-wifi-ota.bin". A version not in that form is
// lowercased and hyphenated in its place. Cut to fit `n`, always ".bin".
void bin_name(const char* version, const char* iso_date, char* out, size_t n);

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
    Dev,          // booted once from stable; a reset ends it unless promoted
};
const char* role_name(Role r);

// Booted by request (New, PendingVerify) is dev; anything else -- Valid, or
// no state at all after a USB flash -- is stable.
Role role_of(SlotState running);

// What the slot that is not running holds, for the screen and to decide
// whether "boot dev once" is offered. An Aborted dev is normal — every dev
// run ends that way — so it is still Ready.
enum class Other : uint8_t { Unknown = 0, Empty, Ready, Invalid };
Other other_from(bool has_valid_image, SlotState state);

// --- installing ----------------------------------------------------------

enum class Install : uint8_t { Idle = 0, Checking, Writing, Done, Failed };

enum class InstallError : uint8_t {
    None = 0,
    Moving,        // refused: not parked
    NotStable,     // refused: only stable writes the dev slot
    NotDev,        // refused: promote is done from the dev image it keeps
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
