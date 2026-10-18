#include "check.hpp"
#include "core/media/feed.hpp"

#include <cstring>
#include <string>

using namespace core::media;
using core::state::Media;

namespace {
char feed(Media& m, const std::string& line, uint32_t now) {
    return apply_line(m, reinterpret_cast<const uint8_t*>(line.data()),
                      line.size(), now);
}
}  // namespace

int main() {
    CASE("each text tag lands in its own field");
    {
        Media m;
        CHECK(feed(m, "T The Devil In I", 100) == 'T');
        CHECK(feed(m, "A Slipknot\n", 101) == 'A');
        CHECK(feed(m, "L .5: The Gray Chapter", 102) == 'L');
        CHECK(feed(m, "Y Undo these chains", 103) == 'Y');
        CHECK(std::strcmp(m.title, "The Devil In I") == 0);
        CHECK(std::strcmp(m.artist, "Slipknot") == 0);
        CHECK(std::strcmp(m.album, ".5: The Gray Chapter") == 0);
        CHECK(std::strcmp(m.lyric, "Undo these chains") == 0);
        CHECK(m.updated_ms == 103);
        CHECK(m.src == kSrcPhone);
    }

    CASE("the separator space is optional and CRLF is stripped");
    {
        Media m;
        feed(m, "TFoo\r\n", 1);
        CHECK(std::strcmp(m.title, "Foo") == 0);
    }

    CASE("unknown tags and empty writes are ignored, not applied");
    {
        Media m;
        CHECK(feed(m, "Q whatever", 5) == 0);
        CHECK(feed(m, "", 5) == 0);
        CHECK(feed(m, "\n", 5) == 0);
        CHECK(m.updated_ms == 0);
        CHECK(m.src == kSrcNone);
    }

    CASE("a 300-byte title is cut to the buffer and terminated");
    {
        Media m;
        feed(m, "T " + std::string(300, 'x'), 1);
        CHECK(std::strlen(m.title) == sizeof(m.title) - 1);
    }

    CASE("a cut never ends inside a UTF-8 character");
    {
        // 58 ASCII bytes then a 3-byte character: only 1 byte of it fits.
        Media m;
        feed(m, "T " + std::string(58, 'a') + "\xE2\x82\xAC", 1);
        CHECK(std::strlen(m.title) == 58);
        // Exactly fitting is kept whole.
        Media n;
        feed(n, "T " + std::string(56, 'a') + "\xE2\x82\xAC", 1);
        CHECK(std::strlen(n.title) == 59);
    }

    CASE("an embedded NUL ends the field");
    {
        Media m;
        const uint8_t raw[] = {'A', ' ', 'a', 'b', 0, 'c'};
        apply_line(m, raw, sizeof(raw), 1);
        CHECK(std::strcmp(m.artist, "ab") == 0);
    }

    CASE("a new title resets the clock and clears the old lyric");
    {
        Media m;
        feed(m, "T One", 10);
        feed(m, "D 200000", 11);
        feed(m, "P 50000", 12);
        feed(m, "Y line of song one", 13);
        feed(m, "T Two", 500);
        CHECK(m.position_ms == 0);
        CHECK(m.duration_ms == 0);
        CHECK(m.position_at_ms == 500);
        CHECK(m.lyric[0] == '\0');
    }

    CASE("the same title resent is a liveness beat, not a track change");
    {
        Media m;
        feed(m, "T One", 10);
        feed(m, "P 50000", 12);
        feed(m, "Y kept", 13);
        feed(m, "T One", 900);
        CHECK(m.position_ms == 50000);
        CHECK(m.position_at_ms == 12);
        CHECK(std::strcmp(m.lyric, "kept") == 0);
    }

    CASE("pausing folds the played time into the anchor");
    {
        Media m;
        feed(m, "S 1", 1000);
        feed(m, "P 0", 1000);
        feed(m, "S 2", 4000);
        CHECK(m.state == 2);
        CHECK(m.position_ms == 3000);
        CHECK(m.position_at_ms == 4000);
        // Resuming restarts the anchor, so the pause is not counted.
        feed(m, "S 1", 9000);
        CHECK(m.position_ms == 3000);
        CHECK(m.position_at_ms == 9000);
    }

    CASE("S 1 resent while playing does not throw away elapsed time");
    {
        Media m;
        feed(m, "S 1", 1000);
        feed(m, "P 0", 1000);
        feed(m, "S 1", 6000);
        CHECK(m.position_at_ms == 1000);
        CHECK(m.position_ms == 0);
    }

    CASE("an out-of-range state is rejected");
    {
        Media m;
        CHECK(feed(m, "S 7", 1) == 0);
        CHECK(feed(m, "S", 1) == 0);
        CHECK(m.state == 0);
    }

    CASE("D and P parse digits and saturate instead of wrapping");
    {
        Media m;
        feed(m, "D 215000ms", 1);
        CHECK(m.duration_ms == 215000);
        feed(m, "P 99999999999999", 1);
        CHECK(m.position_ms == UINT32_MAX);
        feed(m, "D junk", 1);
        CHECK(m.duration_ms == 0);
    }

    CASE("a line at t=0 still marks media as present");
    {
        Media m;
        feed(m, "T x", 0);
        CHECK(m.updated_ms != 0);
    }

    CASE("ctrl lines are C, space, digit, and nothing else is sent");
    {
        uint8_t out[3] = {};
        CHECK(ctrl_line(1, out) == 3);
        CHECK(out[0] == 'C' && out[1] == ' ' && out[2] == '1');
        CHECK(ctrl_line(2, out) == 3 && out[2] == '2');
        CHECK(ctrl_line(3, out) == 0);
        CHECK(ctrl_line(0xFF, out) == 0);
    }

    CASE("N: a message is kind, who and text, and moves seq");
    {
        core::state::Notify n;
        const char line[] = "NM Alex\x1fRunning late, start without me";
        CHECK(core::media::apply_notify(n, reinterpret_cast<const uint8_t*>(line),
                                        sizeof line - 1, 500));
        CHECK(n.kind == 'M');
        CHECK(std::strcmp(n.who, "Alex") == 0);
        CHECK(std::strcmp(n.text, "Running late, start without me") == 0);
        CHECK(n.seq == 1);
        CHECK(n.at_ms == 500);
        CHECK(!n.ringing);
        // Not media: apply_line leaves N alone, and apply_notify leaves T.
        Media m;
        CHECK(apply_line(m, reinterpret_cast<const uint8_t*>(line), sizeof line - 1, 1) == 0);
        const char t[] = "T Song";
        CHECK(!core::media::apply_notify(n, reinterpret_cast<const uint8_t*>(t), sizeof t - 1, 1));
        CHECK(n.seq == 1);
    }

    CASE("N: a call rings until E, or until the timeout");
    {
        core::state::Notify n;
        const auto put = [&](const char* s, uint32_t at) {
            return core::media::apply_notify(n, reinterpret_cast<const uint8_t*>(s),
                                             std::strlen(s), at);
        };
        CHECK(put("NC Mum", 1000));
        CHECK(n.kind == 'C' && n.ringing && n.text[0] == '\0');
        CHECK(core::media::ringing(n, 1000 + 30000));
        CHECK(!core::media::ringing(n, 1000 + core::media::kRingTimeoutMs));
        CHECK(put("NE", 5000));
        CHECK(!n.ringing);
        CHECK(std::strcmp(n.who, "Mum") == 0);   // still says who it was
        CHECK(n.seq == 1);                        // an end is not a new one
        CHECK(put("NX Mum", 6000));
        CHECK(n.kind == 'X' && !n.ringing && n.seq == 2);
    }

    CASE("N: unknown kinds, short lines and control bytes");
    {
        core::state::Notify n;
        const auto put = [&](const char* s, size_t len) {
            return core::media::apply_notify(n, reinterpret_cast<const uint8_t*>(s), len, 1);
        };
        CHECK(!put("NZ who", 6));
        CHECK(!put("N", 1));
        CHECK(n.seq == 0);
        const char tabs[] = "NO Gmail\x1fline one\nline two\tend\n";
        CHECK(put(tabs, sizeof tabs - 1));
        CHECK(std::strcmp(n.text, "line one line two end") == 0);
        // Cut to fit, on a character boundary: 95 bytes of text, then an é.
        char big[200] = "NM x\x1f";
        size_t k = std::strlen(big);
        for (int i = 0; i < 94; ++i) big[k++] = 'a';
        big[k++] = static_cast<char>(0xC3);
        big[k++] = static_cast<char>(0xA9);
        CHECK(put(big, k));
        CHECK(std::strlen(n.text) == 94);
        CHECK(core::media::kMaxNotifyLine == 3 + 31 + 1 + 95);
    }

    return check::report("media");
}
