#include "check.hpp"
#include "core/wifi/wifi.hpp"

#include <cstring>

using namespace core::wifi;

int main() {
    CASE("the password is eight digits and changes with the random words");
    {
        char a[kPassLen + 1], b[kPassLen + 1];
        make_password(1234, 5678, a);
        CHECK(std::strcmp(a, "12345678") == 0);
        make_password(0, 7, b);
        CHECK(std::strcmp(b, "00000007") == 0);   // leading zeros kept
        make_password(0xFFFFFFFFu, 0xFFFFFFFFu, b);
        CHECK(std::strlen(b) == kPassLen);
        for (size_t i = 0; i < kPassLen; ++i) CHECK(b[i] >= '0' && b[i] <= '9');
    }

    CASE("the hotspot goes off when idle, and at once when the bike moves");
    {
        const uint32_t t0 = 1000;
        CHECK(!should_stop(t0 + 1, t0, 0.0f));
        CHECK(!should_stop(t0 + kIdleOffMs - 1, t0, 0.0f));
        CHECK(should_stop(t0 + kIdleOffMs, t0, 0.0f));
        CHECK(should_stop(t0 + 1, t0, 20.0f));
        CHECK(!should_stop(t0 + 1, t0, kMovingMph));    // walking it about
        // A counter that wrapped since the last use is a short gap.
        CHECK(!should_stop(5, 0xFFFFFF00u, 0.0f));
        // A page request on the other core stamped its use just after the
        // wifi task read the clock: in use, not 49 days idle.
        CHECK(!should_stop(t0, t0 + 2, 0.0f));
    }

    CASE("the countdown rounds up and stops at zero");
    {
        CHECK(off_in_s(0, 0) == kIdleOffMs / 1000u);
        CHECK(off_in_s(kIdleOffMs - 1, 0) == 1);
        CHECK(off_in_s(kIdleOffMs, 0) == 0);
        CHECK(off_in_s(kIdleOffMs + 5000, 0) == 0);
        CHECK(off_in_s(100, 102) == kIdleOffMs / 1000u);   // used just after `now`
    }

    CASE("only plain .csv names in logs/ can be downloaded");
    {
        CHECK(valid_log_name("s000.csv"));
        CHECK(valid_log_name("s123.csv"));
        CHECK(valid_log_name("ride_2-b.csv"));
        CHECK(!valid_log_name(nullptr));
        CHECK(!valid_log_name(""));
        CHECK(!valid_log_name(".csv"));
        CHECK(!valid_log_name("../dash.bin"));
        CHECK(!valid_log_name("..csv"));
        CHECK(!valid_log_name("a/b.csv"));
        CHECK(!valid_log_name("a\\b.csv"));
        CHECK(!valid_log_name("s000.csv.bak"));
        CHECK(!valid_log_name("s000.txt"));
        CHECK(!valid_log_name("s 000.csv"));
        CHECK(!valid_log_name("s%2e.csv"));
        CHECK(!valid_log_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.csv"));
    }

    CASE("game files in games/ are .wad in either case, and nothing else");
    {
        CHECK(valid_game_name("doom1.wad"));
        CHECK(valid_game_name("DOOM1.WAD"));
        CHECK(valid_game_name("freedoom-2.Wad"));
        CHECK(!valid_game_name(nullptr));
        CHECK(!valid_game_name(""));
        CHECK(!valid_game_name(".wad"));
        CHECK(!valid_game_name("../doom1.wad"));
        CHECK(!valid_game_name("a/doom1.wad"));
        CHECK(!valid_game_name("doom1.wad.part"));
        CHECK(!valid_game_name("doom1.csv"));
        CHECK(!valid_game_name("default.cfg"));
        CHECK(!valid_game_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.wad"));
        // and the log rule stayed case-exact
        CHECK(!valid_log_name("S000.CSV"));
    }

    CASE("every state has a name");
    {
        CHECK(std::strcmp(state_name(State::On), "on") == 0);
        CHECK(std::strcmp(state_name(State::UpdatingC6), "updating C6") == 0);
    }

    return check::report("wifi");
}
