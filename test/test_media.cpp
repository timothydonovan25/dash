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

    return check::report("media");
}
