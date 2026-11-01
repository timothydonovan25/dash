// An iPhone's own accessory services (§11.1.1): AMS for what is playing,
// ANCS for calls and messages. No app on the phone: iOS serves both over
// GATT to any bonded accessory, and the dash reads them as a client.
//
// Everything here turns what those services send into the same Media and
// Notify state the Dash Media feed fills, by building §11.1 lines and handing
// them to apply_line() and apply_notify(). So the bounds, the UTF-8 cuts and
// the "new title restarts the clock" rule are the feed's, not a second copy.
// The `ble` service does the radio calls and hands each notification's bytes
// here; it does no parsing of its own.
//
// The phone is untrusted input (§11.8), the same as the feed.
#pragma once
#include "core/media/feed.hpp"
#include "core/state/vehicle.hpp"
#include <cstddef>
#include <cstdint>

namespace core::media {

// `Media.src` for media that came from AMS rather than the feed.
inline constexpr uint8_t kSrcAms = 2;

namespace ams {

// Apple's UUIDs, in the little-endian byte order GATT puts on the air (the
// reverse of the string form, which test_apple checks).
inline constexpr uint8_t kService[16] = {   // 89D3502B-0F36-433A-8EF4-C502AD55F8DC
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
    0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89};
inline constexpr uint8_t kRemoteCommand[16] = {   // 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8,
    0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B};
inline constexpr uint8_t kEntityUpdate[16] = {    // 2F7CABCE-808D-411F-9A0C-BB92BA96C102
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A,
    0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F};

inline constexpr uint8_t kEntityPlayer = 0;
inline constexpr uint8_t kEntityTrack = 2;
inline constexpr uint8_t kPlayerPlaybackInfo = 1;
inline constexpr uint8_t kTrackArtist = 0;
inline constexpr uint8_t kTrackAlbum = 1;
inline constexpr uint8_t kTrackTitle = 2;
inline constexpr uint8_t kTrackDuration = 3;

// The two Entity Update writes that subscribe, in this order. Title first:
// iOS sends a track's attributes in the order they were asked for, and a new
// title clears the duration (apply_line's 'T'), so a duration asked for
// before the title would be wiped by its own track's title.
inline constexpr uint8_t kSubscribeTrack[] = {kEntityTrack, kTrackTitle, kTrackArtist,
                                              kTrackAlbum, kTrackDuration};
inline constexpr uint8_t kSubscribePlayer[] = {kEntityPlayer, kPlayerPlaybackInfo};

// One Entity Update notification: entity, attribute, flags, then the value
// as UTF-8 (iOS cuts a long one to fit the MTU and sets a flag, which is
// ignored: every field here is shorter than any MTU anyway). Returns the
// §11.1 tag it amounted to ('T', 'A', 'L', 'D' or 'S'), or 0 if it was
// something the dash does not show.
char apply_update(state::Media& m, const uint8_t* data, size_t len, uint32_t now_ms);

// A handlebar press (ctrl_line's 0 previous, 1 play/pause, 2 next) as an
// AMS Remote Command id. False for any other value.
bool remote_command(uint8_t cmd, uint8_t& out);

}  // namespace ams

namespace ancs {

inline constexpr uint8_t kService[16] = {   // 7905F431-B5CE-4E99-A40F-4B1E122D00D0
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79};
inline constexpr uint8_t kNotificationSource[16] = {   // 9FBF120D-6301-42D9-8C58-25E699A21DBD
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F};
inline constexpr uint8_t kControlPoint[16] = {   // 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69};
inline constexpr uint8_t kDataSource[16] = {   // 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22};

inline constexpr uint8_t kEventAdded = 0;
inline constexpr uint8_t kEventRemoved = 2;
inline constexpr uint8_t kFlagPreExisting = 1u << 2;
inline constexpr uint8_t kCategoryIncomingCall = 1;
inline constexpr uint8_t kCategoryMissedCall = 2;
inline constexpr uint8_t kCategorySocial = 4;   // Messages, and chat apps
inline constexpr uint8_t kAttrTitle = 1;
inline constexpr uint8_t kAttrMessage = 3;

// Asked for no longer than Notify can hold.
inline constexpr uint16_t kTitleMax = sizeof(state::Notify::who) - 1;
inline constexpr uint16_t kMessageMax = sizeof(state::Notify::text) - 1;

// Get Notification Attributes: command, UID, and two attributes with their
// maximum lengths.
inline constexpr size_t kRequestLen = 1 + 4 + 3 + 3;
// Its whole answer on the Data Source, which may come in several pieces.
inline constexpr size_t kMaxResponse = 1 + 4 + (3 + kTitleMax) + (3 + kMessageMax);

// A request the phone never answers is given up after this long, so one
// lost reply does not stop every notification after it.
inline constexpr uint32_t kReplyMs = 5000;

// Calls, missed calls and messages, as the Android app sends them (and
// nothing else, for the same reason: a dash that lights up for every app
// teaches the rider to ignore it). ANCS says only that a notification
// exists; its text is asked for, one notification at a time, and arrives on
// another characteristic. This holds the ones waiting to be asked about and
// puts each answer back together.
class Tracker {
public:
    void reset();

    // One Notification Source notification. True if `n` changed: the
    // ringing call went away, which needs nothing asked.
    bool on_source(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms);

    // One Data Source notification, a piece of an answer. True once an
    // answer is whole and has landed in `n`.
    bool on_data(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms);

    // The next request to write to the Control Point, if one is due: fills
    // `out` and returns its length (kRequestLen), else 0. Only one is out at
    // a time, and one unanswered for kReplyMs is dropped.
    size_t next_request(uint8_t* out, size_t cap, uint32_t now_ms);

    // The write of the last request did not start: ask again next time.
    void unask() { asked_ = false; }

    size_t pending() const { return n_; }

private:
    struct Pending {
        uint32_t uid;
        char     kind;   // Notify::kind
    };
    static constexpr size_t kQueue = 4;

    void pop();

    Pending  q_[kQueue] = {};
    size_t   n_ = 0;
    bool     asked_ = false;
    uint32_t asked_ms_ = 0;
    uint8_t  buf_[kMaxResponse] = {};
    size_t   got_ = 0;
    bool     ringing_known_ = false;
    uint32_t ringing_uid_ = 0;
};

}  // namespace ancs
}  // namespace core::media
