#include "core/media/apple.hpp"

#include <cstring>

namespace core::media {
namespace {

// "245.123" seconds as decimal ASCII milliseconds, which is what §11.1's
// `D` and `P` carry. Stops at the first character that is neither a digit
// nor the one point, keeps at most three decimals, and saturates.
size_t seconds_to_ms(const uint8_t* s, size_t len, char* out, size_t cap) {
    uint64_t ms = 0;
    bool point = false;
    int decimals = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '.' && !point) { point = true; continue; }
        if (s[i] < '0' || s[i] > '9') break;
        if (point) {
            if (decimals == 3) continue;
            ++decimals;
        }
        ms = ms * 10 + static_cast<uint64_t>(s[i] - '0');
        if (ms > UINT32_MAX) { ms = UINT32_MAX; break; }
    }
    for (; decimals < 3 && ms <= UINT32_MAX; ++decimals) ms *= 10;
    if (ms > UINT32_MAX) ms = UINT32_MAX;
    char digits[11];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + ms % 10);
        ms /= 10;
    } while (ms != 0);
    size_t k = 0;
    while (n > 0 && k + 1 < cap) out[k++] = digits[--n];
    out[k] = '\0';
    return k;
}

char feed_line(state::Media& m, char tag, const char* v, size_t vlen, uint32_t now_ms) {
    char line[1 + sizeof(state::Media::title)];
    line[0] = tag;
    std::memcpy(line + 1, v, vlen);
    return apply_line(m, reinterpret_cast<const uint8_t*>(line), 1 + vlen, now_ms);
}

}  // namespace

namespace ams {

char apply_update(state::Media& m, const uint8_t* data, size_t len, uint32_t now_ms) {
    if (len < 3) return 0;
    const uint8_t entity = data[0];
    const uint8_t attr = data[1];
    const uint8_t* v = data + 3;
    const size_t vlen = len - 3;

    // Cut to fit on a UTF-8 boundary before the tag goes on, so the line
    // apply_line sees is already short enough that it never cuts again.
    char text[sizeof(state::Media::title)];
    char tag = 0;
    size_t n = 0;
    if (entity == kEntityTrack) {
        switch (attr) {
            case kTrackTitle:  tag = 'T'; break;
            case kTrackArtist: tag = 'A'; break;
            case kTrackAlbum:  tag = 'L'; break;
            case kTrackDuration:
                tag = 'D';
                n = seconds_to_ms(v, vlen, text, sizeof text);
                break;
            default: return 0;
        }
        if (tag != 'D') n = copy_field(text, sizeof text, v, vlen);
        if (!feed_line(m, tag, text, n, now_ms)) return 0;
    } else if (entity == kEntityPlayer && attr == kPlayerPlaybackInfo) {
        // "state,rate,elapsed": 0 paused, 1 playing, 2 rewinding, 3 fast
        // forwarding. Empty when no app is playing anything.
        char s = '0';
        if (vlen > 0) s = v[0] == '0' ? '2' : (v[0] >= '1' && v[0] <= '3') ? '1' : '0';
        // State first, then the elapsed time: entering play restarts the
        // anchor and leaving it freezes it, and the phone's own elapsed time
        // then replaces whatever that folding worked out.
        if (!feed_line(m, 'S', &s, 1, now_ms)) return 0;
        const uint8_t* comma = static_cast<const uint8_t*>(std::memchr(v, ',', vlen));
        if (comma) comma = static_cast<const uint8_t*>(
                       std::memchr(comma + 1, ',', vlen - static_cast<size_t>(comma + 1 - v)));
        if (comma) {
            const size_t at = static_cast<size_t>(comma + 1 - v);
            n = seconds_to_ms(v + at, vlen - at, text, sizeof text);
            feed_line(m, 'P', text, n, now_ms);
        }
        tag = 'S';
    } else {
        return 0;
    }
    m.src = kSrcAms;
    return tag;
}

bool remote_command(uint8_t cmd, uint8_t& out) {
    // AMS: 2 toggle play/pause, 3 next track, 4 previous track.
    switch (cmd) {
        case 0: out = 4; return true;
        case 1: out = 2; return true;
        case 2: out = 3; return true;
        default: return false;
    }
}

}  // namespace ams

namespace ancs {
namespace {

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

char kind_of(uint8_t category) {
    switch (category) {
        case kCategoryIncomingCall: return 'C';
        case kCategoryMissedCall:   return 'X';
        case kCategorySocial:       return 'M';
        default:                    return 0;
    }
}

}  // namespace

void Tracker::reset() { *this = Tracker{}; }

void Tracker::pop() {
    if (n_ == 0) return;
    std::memmove(q_, q_ + 1, (n_ - 1) * sizeof q_[0]);
    --n_;
    asked_ = false;
    got_ = 0;
}

bool Tracker::on_source(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms) {
    if (len < 8) return false;
    const uint8_t event = data[0];
    const uint8_t flags = data[1];
    const uint32_t uid = le32(data + 4);

    if (event == kEventRemoved) {
        // Gone before it was shown: forgotten, and if it is the one being
        // asked about, its answer is not waited for (a late one fails the
        // UID check). Otherwise a call hung up that fast would ring on.
        for (size_t i = 0; i < n_; ++i) {
            if (q_[i].uid != uid) continue;
            if (i == 0) {
                pop();
            } else {
                std::memmove(q_ + i, q_ + i + 1, (n_ - i - 1) * sizeof q_[0]);
                --n_;
            }
            break;
        }
        // Answered, declined or hung up: the same as the feed's 'E'.
        if (ringing_known_ && uid == ringing_uid_) {
            ringing_known_ = false;
            static const uint8_t kEnded[] = {'N', 'E'};
            return apply_notify(n, kEnded, sizeof kEnded, now_ms);
        }
        return false;
    }
    if (event != kEventAdded) return false;   // Modified: a thread re-posted
    const char kind = kind_of(data[2]);
    if (kind == 0) return false;
    // On subscribing, iOS replays everything already in Notification
    // Centre. Old messages are not news; a call ringing right now is.
    if ((flags & kFlagPreExisting) && kind != 'C') return false;
    if (n_ == kQueue) return false;   // a burst: the newest wait their turn on the phone
    q_[n_++] = Pending{uid, kind};
    return false;
}

size_t Tracker::next_request(uint8_t* out, size_t cap, uint32_t now_ms) {
    if (asked_ && now_ms - asked_ms_ >= kReplyMs) pop();
    if (asked_ || n_ == 0 || cap < kRequestLen) return 0;
    const uint32_t uid = q_[0].uid;
    size_t k = 0;
    out[k++] = 0;   // Get Notification Attributes
    for (int i = 0; i < 4; ++i) out[k++] = static_cast<uint8_t>(uid >> (8 * i));
    out[k++] = kAttrTitle;
    out[k++] = static_cast<uint8_t>(kTitleMax);
    out[k++] = static_cast<uint8_t>(kTitleMax >> 8);
    out[k++] = kAttrMessage;
    out[k++] = static_cast<uint8_t>(kMessageMax);
    out[k++] = static_cast<uint8_t>(kMessageMax >> 8);
    asked_ = true;
    asked_ms_ = now_ms;
    got_ = 0;
    return k;
}

bool Tracker::on_data(state::Notify& n, const uint8_t* data, size_t len, uint32_t now_ms) {
    if (!asked_) return false;
    if (len > sizeof buf_ - got_) {   // longer than was asked for: not ours
        pop();
        return false;
    }
    std::memcpy(buf_ + got_, data, len);
    got_ += len;
    if (got_ < 5) return false;
    if (buf_[0] != 0 || le32(buf_ + 1) != q_[0].uid) {
        // A late answer to a request already given up on. Wait for ours.
        got_ = 0;
        return false;
    }

    const uint8_t* title = nullptr;
    const uint8_t* message = nullptr;
    size_t title_len = 0, message_len = 0;
    size_t at = 5;
    for (int attrs = 0; attrs < 2; ++attrs) {
        if (got_ - at < 3) return false;   // the rest is still on its way
        const uint8_t id = buf_[at];
        const size_t alen = static_cast<size_t>(buf_[at + 1]) | static_cast<size_t>(buf_[at + 2]) << 8;
        if (got_ - at - 3 < alen) return false;
        if (id == kAttrTitle) { title = buf_ + at + 3; title_len = alen; }
        if (id == kAttrMessage) { message = buf_ + at + 3; message_len = alen; }
        at += 3 + alen;
    }

    // §11.1's `N` line: kind, the caller or sender, US, and the text, for a
    // message only. A US inside the title would split it early.
    const char kind = q_[0].kind;
    uint8_t line[3 + kMaxResponse];
    size_t k = 0;
    line[k++] = 'N';
    line[k++] = static_cast<uint8_t>(kind);
    line[k++] = ' ';
    for (size_t i = 0; i < title_len; ++i) line[k++] = title[i] == 0x1F ? ' ' : title[i];
    if (kind == 'M' && message_len > 0) {
        line[k++] = 0x1F;
        std::memcpy(line + k, message, message_len);
        k += message_len;
    }
    const uint32_t uid = q_[0].uid;
    pop();
    if (!apply_notify(n, line, k, now_ms)) return false;
    if (kind == 'C') {
        ringing_known_ = true;
        ringing_uid_ = uid;
    }
    return true;
}

}  // namespace ancs
}  // namespace core::media
