// The Dash Media feed (§11.1): one GATT write is one line, tag byte first.
//
// Everything about the wire format lives here, away from NimBLE, so that a
// malformed line from the phone is a unit test rather than a ride. The `ble`
// service hands each write's bytes to `apply_line()` and publishes the
// result; it does no parsing of its own.
//
// The phone is untrusted input (§11.8). Every field is bounded to its buffer
// and terminated, and a string cut to fit is cut on a UTF-8 character
// boundary, so a long title never leaves half a multibyte character for the
// font to draw as garbage.
#pragma once
#include "core/nav/nav.hpp"
#include "core/state/vehicle.hpp"
#include <cstddef>
#include <cstdint>

namespace core::media {

// How long a title stays on screen after the phone drops (§11.8). Long
// enough to ride through a link that blinks out and straight back; short
// enough that a phone left at home does not leave a song up for the ride.
inline constexpr uint32_t kDisconnectHoldMs = 10000;

// `Media.src` values.
inline constexpr uint8_t kSrcNone  = 0;
inline constexpr uint8_t kSrcPhone = 1;

// Applies one Feed write to `m`. `now_ms` is the dash clock, recorded as the
// anchor for `P` and stamped into `updated_ms`. Returns the tag if the line
// was accepted, or 0 if it was empty or carried a tag this firmware does not
// know, which is ignored rather than being an error.
char apply_line(state::Media& m, const uint8_t* data, size_t len,
                uint32_t now_ms);

// §11.1 tag `N`: a notification from the phone, in one write.
//
//     N<kind> <who><US><text>
//
// `kind` is one byte: 'C' an incoming call (ringing), 'E' that call ended
// (answered, rejected or gone), 'X' a missed call, 'M' a message, 'O' any
// other app. `who` is the caller, sender or app name, `US` is 0x1F (ASCII's
// unit separator), and `text` the message, absent for a call. Cut to fit on
// a UTF-8 boundary like every field; control bytes inside either become
// spaces, since both are drawn. An 'E' changes nothing but `ringing`, so a
// call that has stopped still says who it was.
//
// Returns true if the line was an `N` this firmware understands; `n` is
// untouched otherwise. Separate from apply_line() because a notification is
// not media: it lives in its own slot and survives a new song.
bool apply_notify(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms);

// §11.1 tag `B`: a pod button pressed on the phone, for a dash whose pod is
// gone. `B A`..`B E` a short press, `B a`..`B e` a long one; already decoded,
// so a dropped write loses one press and never holds a button down.
// Returns false, `e` untouched, for anything else.
bool parse_button(const uint8_t* data, size_t len, nav::InputEvent& e);

// A call stops ringing when the phone says so, or after this long without
// hearing: a phone that drops out mid-ring must not ring on the dash forever.
inline constexpr uint32_t kRingTimeoutMs = 45000;
bool ringing(const state::Notify& n, uint32_t now_ms);

// The longest `N` write the phone sends: tag, kind, space, `who`, US,
// `text`. The `ble` task's line buffer is sized from this.
inline constexpr size_t kMaxNotifyLine =
    3 + (sizeof(state::Notify::who) - 1) + 1 + (sizeof(state::Notify::text) - 1);

// Copies at most `cap - 1` bytes of `src` into `dst` and terminates it,
// backing off so the copy never ends inside a UTF-8 sequence. Returns the
// number of bytes copied.
size_t copy_field(char* dst, size_t cap, const uint8_t* src, size_t len);

// The dash-to-phone Ctrl notify: "C 0" previous, "C 1" play/pause, "C 2"
// next. Writes three bytes to `out` and returns 3, or returns 0 for any
// other command, which is not sent.
size_t ctrl_line(uint8_t cmd, uint8_t out[3]);

}  // namespace core::media
