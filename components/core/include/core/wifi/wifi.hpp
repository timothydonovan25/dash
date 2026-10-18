// On-demand Wi-Fi for updates and downloads (§11.7).
//
// The dash is a hotspot, never a station: it works at a track with no
// router and stores no passwords. The rider turns it on from DIAG -> Update
// or CONFIG, joins "CRF250F-Dash" with the password the screen shows, and
// opens the dash's page. It turns itself off when nobody has used it for a
// while, and as soon as the bike moves -- a radio nobody is using is power
// and attack surface for nothing.
//
// This is the policy half; `services/wifi_service` does the radio and the
// web server.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::wifi {

inline constexpr const char* kSsid = "CRF250F-Dash";

// Long enough to fetch a day's logs on a slow phone, short enough that a
// forgotten hotspot does not run the battery down overnight.
inline constexpr uint32_t kIdleOffMs = 10u * 60u * 1000u;

// Above this the bike is moving and the hotspot goes off.
inline constexpr float kMovingMph = 3.0f;

// WPA2 wants 8 to 63 characters. Eight digits, a new set every session:
// easy to type on a phone from the screen, and a stranger who saw last
// week's has nothing.
inline constexpr size_t kPassLen = 8;

// Writes kPassLen digits and a terminator from two random words. `out`
// must hold kPassLen + 1.
void make_password(uint32_t r0, uint32_t r1, char* out);

// Whether a running hotspot should go off now. `last_use_ms` is when it
// started or when the page was last asked for anything, whichever is later.
bool should_stop(uint32_t now_ms, uint32_t last_use_ms, float speed_mph);

// Whole seconds until the idle timer turns it off; 0 once it is due.
uint32_t off_in_s(uint32_t now_ms, uint32_t last_use_ms);

// A log file name the page may ask for: a plain file in logs/, ending .csv,
// made of letters, digits, '-', '_' and one '.'. Anything else -- a slash,
// "..", an empty name -- is refused, so the URL cannot reach outside the
// logs directory.
bool valid_log_name(const char* name);

// The same rule for a game's data file in games/, ending .wad in either
// case (DOOM1.WAD and doom1.wad are both what people have), for the page's
// upload and delete on a build that runs one.
bool valid_game_name(const char* name);

// What the Update screen shows on the Wi-Fi row.
enum class State : uint8_t {
    Off = 0,
    Starting,
    On,
    Failed,        // the C6 would not start the hotspot
    UpdatingC6,    // the C6 had no Wi-Fi; its firmware is being replaced
};

const char* state_name(State s);

}  // namespace core::wifi
