#include "check.hpp"
#include "core/media/apple.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace core::media;
using core::state::Media;
using core::state::Notify;

namespace {

// "89D3502B-0F36-..." to the little-endian bytes GATT carries.
bool uuid_is(const uint8_t (&le)[16], const char* s) {
    uint8_t be[16];
    int n = 0;
    for (const char* p = s; *p && n < 32; ++p) {
        if (*p == '-') continue;
        const int v = *p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10;
        if (n % 2 == 0) be[n / 2] = static_cast<uint8_t>(v << 4);
        else be[n / 2] |= static_cast<uint8_t>(v);
        ++n;
    }
    for (int i = 0; i < 16; ++i) {
        if (le[i] != be[15 - i]) return false;
    }
    return n == 32;
}

char update(Media& m, uint8_t entity, uint8_t attr, const std::string& value, uint32_t now) {
    std::vector<uint8_t> b = {entity, attr, 0};
    b.insert(b.end(), value.begin(), value.end());
    return ams::apply_update(m, b.data(), b.size(), now);
}

std::vector<uint8_t> source(uint8_t event, uint8_t flags, uint8_t category, uint32_t uid) {
    return {event, flags, category, 1,
            static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8),
            static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 24)};
}

std::vector<uint8_t> answer(uint32_t uid, const std::string& title, const std::string& message) {
    std::vector<uint8_t> b = {0, static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8),
                              static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 24)};
    b.push_back(ancs::kAttrTitle);
    b.push_back(static_cast<uint8_t>(title.size()));
    b.push_back(0);
    b.insert(b.end(), title.begin(), title.end());
    b.push_back(ancs::kAttrMessage);
    b.push_back(static_cast<uint8_t>(message.size()));
    b.push_back(0);
    b.insert(b.end(), message.begin(), message.end());
    return b;
}

}  // namespace

int main() {
    CASE("the UUID bytes are Apple's strings, reversed");
    {
        CHECK(uuid_is(ams::kService, "89D3502B-0F36-433A-8EF4-C502AD55F8DC"));
        CHECK(uuid_is(ams::kRemoteCommand, "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2"));
        CHECK(uuid_is(ams::kEntityUpdate, "2F7CABCE-808D-411F-9A0C-BB92BA96C102"));
        CHECK(uuid_is(ancs::kService, "7905F431-B5CE-4E99-A40F-4B1E122D00D0"));
        CHECK(uuid_is(ancs::kNotificationSource, "9FBF120D-6301-42D9-8C58-25E699A21DBD"));
        CHECK(uuid_is(ancs::kControlPoint, "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"));
        CHECK(uuid_is(ancs::kDataSource, "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"));
    }

    CASE("track attributes land in the media fields, marked as AMS");
    {
        Media m;
        CHECK(update(m, ams::kEntityTrack, ams::kTrackTitle, "Stranded", 100) == 'T');
        CHECK(update(m, ams::kEntityTrack, ams::kTrackArtist, "Gojira", 101) == 'A');
        CHECK(update(m, ams::kEntityTrack, ams::kTrackAlbum, "Magma", 102) == 'L');
        CHECK(update(m, ams::kEntityTrack, ams::kTrackDuration, "271.493", 103) == 'D');
        CHECK(std::strcmp(m.title, "Stranded") == 0);
        CHECK(std::strcmp(m.artist, "Gojira") == 0);
        CHECK(std::strcmp(m.album, "Magma") == 0);
        CHECK(m.duration_ms == 271493);
        CHECK(m.src == kSrcAms);
        CHECK(m.updated_ms == 103);
    }

    CASE("durations: whole seconds, extra decimals and junk");
    {
        Media m;
        update(m, ams::kEntityTrack, ams::kTrackDuration, "180", 1);
        CHECK(m.duration_ms == 180000);
        update(m, ams::kEntityTrack, ams::kTrackDuration, "1.23456", 2);
        CHECK(m.duration_ms == 1234);
        update(m, ams::kEntityTrack, ams::kTrackDuration, "99999999999", 3);
        CHECK(m.duration_ms == UINT32_MAX);
        update(m, ams::kEntityTrack, ams::kTrackDuration, "", 4);
        CHECK(m.duration_ms == 0);
    }

    CASE("playback info sets the state and anchors the elapsed time");
    {
        Media m;
        CHECK(update(m, ams::kEntityPlayer, ams::kPlayerPlaybackInfo, "1,1.0,12.5", 1000) == 'S');
        CHECK(m.state == 1);
        CHECK(m.position_ms == 12500);
        CHECK(m.position_at_ms == 1000);
        update(m, ams::kEntityPlayer, ams::kPlayerPlaybackInfo, "0,0.0,20.25", 9000);
        CHECK(m.state == 2);   // paused
        CHECK(m.position_ms == 20250);
        update(m, ams::kEntityPlayer, ams::kPlayerPlaybackInfo, "3,2.0,21.0", 9100);
        CHECK(m.state == 1);   // fast forwarding counts as playing
        update(m, ams::kEntityPlayer, ams::kPlayerPlaybackInfo, "", 9200);
        CHECK(m.state == 0);   // nothing playing
    }

    CASE("a new title restarts the clock, as the feed's does");
    {
        Media m;
        update(m, ams::kEntityTrack, ams::kTrackTitle, "One", 1);
        update(m, ams::kEntityTrack, ams::kTrackDuration, "200", 2);
        update(m, ams::kEntityPlayer, ams::kPlayerPlaybackInfo, "1,1.0,150", 3);
        update(m, ams::kEntityTrack, ams::kTrackTitle, "Two", 4);
        CHECK(m.position_ms == 0);
        CHECK(m.duration_ms == 0);
        update(m, ams::kEntityTrack, ams::kTrackDuration, "300", 5);
        CHECK(m.duration_ms == 300000);
    }

    CASE("a long title is cut on a UTF-8 boundary; short and unknown updates ignored");
    {
        Media m;
        std::string t(58, 'x');
        t += "\xc3\xa9\xc3\xa9";   // 62 bytes, the cut lands inside an é
        update(m, ams::kEntityTrack, ams::kTrackTitle, t, 1);
        CHECK(std::strlen(m.title) == 58);
        const uint8_t two[] = {ams::kEntityTrack, ams::kTrackTitle};
        CHECK(ams::apply_update(m, two, sizeof two, 2) == 0);
        CHECK(update(m, 1, 0, "queue", 3) == 0);
        CHECK(update(m, ams::kEntityPlayer, 2, "0.5", 4) == 0);   // volume
    }

    CASE("handlebar presses map to AMS remote commands");
    {
        uint8_t c = 0xFF;
        CHECK(ams::remote_command(0, c) && c == 4);
        CHECK(ams::remote_command(1, c) && c == 2);
        CHECK(ams::remote_command(2, c) && c == 3);
        CHECK(!ams::remote_command(3, c));
    }

    CASE("a message is asked about, then shows with its sender and text");
    {
        ancs::Tracker t;
        Notify n;
        uint8_t req[ancs::kRequestLen];
        auto s = source(ancs::kEventAdded, 0, ancs::kCategorySocial, 0x01020304);
        CHECK(!t.on_source(n, s.data(), s.size(), 10));
        CHECK(t.next_request(req, sizeof req, 10) == ancs::kRequestLen);
        CHECK(req[0] == 0 && req[1] == 0x04 && req[4] == 0x01);
        CHECK(req[5] == ancs::kAttrTitle && req[6] == ancs::kTitleMax);
        CHECK(req[8] == ancs::kAttrMessage && req[9] == ancs::kMessageMax);
        CHECK(t.next_request(req, sizeof req, 11) == 0);   // one out at a time

        auto a = answer(0x01020304, "Sam", "on my way");
        // In two pieces, as a small MTU sends it.
        CHECK(!t.on_data(n, a.data(), 7, 20));
        CHECK(t.on_data(n, a.data() + 7, a.size() - 7, 21));
        CHECK(n.kind == 'M');
        CHECK(std::strcmp(n.who, "Sam") == 0);
        CHECK(std::strcmp(n.text, "on my way") == 0);
        CHECK(n.seq == 1);
        CHECK(t.pending() == 0);
    }

    CASE("a call rings until iOS removes it, and shows no text");
    {
        ancs::Tracker t;
        Notify n;
        uint8_t req[ancs::kRequestLen];
        auto s = source(ancs::kEventAdded, 0, ancs::kCategoryIncomingCall, 7);
        t.on_source(n, s.data(), s.size(), 0);
        t.next_request(req, sizeof req, 0);
        auto a = answer(7, "Mum", "Incoming Call");
        CHECK(t.on_data(n, a.data(), a.size(), 1));
        CHECK(n.kind == 'C' && n.ringing);
        CHECK(n.text[0] == '\0');
        auto gone = source(ancs::kEventRemoved, 0, ancs::kCategoryIncomingCall, 7);
        CHECK(t.on_source(n, gone.data(), gone.size(), 2));
        CHECK(!n.ringing);
        CHECK(std::strcmp(n.who, "Mum") == 0);
    }

    CASE("a call hung up before its answer never rings");
    {
        ancs::Tracker t;
        Notify n;
        uint8_t req[ancs::kRequestLen];
        auto s = source(ancs::kEventAdded, 0, ancs::kCategoryIncomingCall, 9);
        t.on_source(n, s.data(), s.size(), 0);
        t.next_request(req, sizeof req, 0);
        auto gone = source(ancs::kEventRemoved, 0, ancs::kCategoryIncomingCall, 9);
        t.on_source(n, gone.data(), gone.size(), 1);
        auto a = answer(9, "Mum", "");
        CHECK(!t.on_data(n, a.data(), a.size(), 2));
        CHECK(!n.ringing && n.seq == 0);
    }

    CASE("old notifications and other apps are not shown; an old ringing call is");
    {
        ancs::Tracker t;
        Notify n;
        auto old = source(ancs::kEventAdded, ancs::kFlagPreExisting, ancs::kCategorySocial, 1);
        auto mail = source(ancs::kEventAdded, 0, 6, 2);
        auto ring = source(ancs::kEventAdded, ancs::kFlagPreExisting, ancs::kCategoryIncomingCall, 3);
        t.on_source(n, old.data(), old.size(), 0);
        t.on_source(n, mail.data(), mail.size(), 0);
        CHECK(t.pending() == 0);
        t.on_source(n, ring.data(), ring.size(), 0);
        CHECK(t.pending() == 1);
    }

    CASE("an unanswered request is dropped and the next one asked");
    {
        ancs::Tracker t;
        Notify n;
        uint8_t req[ancs::kRequestLen];
        auto s1 = source(ancs::kEventAdded, 0, ancs::kCategoryMissedCall, 1);
        auto s2 = source(ancs::kEventAdded, 0, ancs::kCategoryMissedCall, 2);
        t.on_source(n, s1.data(), s1.size(), 0);
        t.on_source(n, s2.data(), s2.size(), 0);
        CHECK(t.next_request(req, sizeof req, 0) == ancs::kRequestLen && req[1] == 1);
        CHECK(t.next_request(req, sizeof req, ancs::kReplyMs - 1) == 0);
        CHECK(t.next_request(req, sizeof req, ancs::kReplyMs) == ancs::kRequestLen && req[1] == 2);
        // The first answer, late, is not mistaken for the second's.
        auto late = answer(1, "Old", "");
        CHECK(!t.on_data(n, late.data(), late.size(), ancs::kReplyMs + 1));
        auto a = answer(2, "Jo", "Missed Call");
        CHECK(t.on_data(n, a.data(), a.size(), ancs::kReplyMs + 2));
        CHECK(n.kind == 'X' && std::strcmp(n.who, "Jo") == 0 && n.text[0] == '\0');
    }

    CASE("an answer longer than was asked for is refused, not overrun");
    {
        ancs::Tracker t;
        Notify n;
        uint8_t req[ancs::kRequestLen];
        auto s = source(ancs::kEventAdded, 0, ancs::kCategorySocial, 5);
        t.on_source(n, s.data(), s.size(), 0);
        t.next_request(req, sizeof req, 0);
        std::vector<uint8_t> big(ancs::kMaxResponse + 1, 'x');
        CHECK(!t.on_data(n, big.data(), big.size(), 1));
        CHECK(t.pending() == 0);
    }

    return check::report("apple");
}
