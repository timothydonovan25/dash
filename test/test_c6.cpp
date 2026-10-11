#include "check.hpp"
#include "core/c6/c6.hpp"

using namespace core::c6;

namespace {
FwId id(uint32_t ma, uint32_t mi, uint32_t pa, int32_t b) {
    FwId f;
    f.major = ma; f.minor = mi; f.patch = pa; f.build = b; f.valid = true;
    return f;
}
}  // namespace

int main() {
    CASE("build id: four SHA bytes, positive, never 0");
    {
        const uint8_t a[4] = {0x12, 0x34, 0x56, 0x78};
        CHECK(build_id(a) == 0x12345678);
        const uint8_t hi[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(build_id(hi) == 0x7FFFFFFF);
        const uint8_t zero[4] = {0, 0, 0, 0};
        CHECK(build_id(zero) == 1);           // 0 would arrive as -1
        const uint8_t top[4] = {0x80, 0, 0, 0};
        CHECK(build_id(top) == 1);            // masks to 0, so 1
    }

    CASE("version strings");
    {
        FwId f;
        CHECK(parse_version("2.12.8", f) && f.major == 2 && f.minor == 12 && f.patch == 8);
        CHECK(!parse_version("2.12", f));
        CHECK(!parse_version("2.12.8.1", f));
        CHECK(!parse_version("v2.12.8", f));
        CHECK(!parse_version("2..8", f));
        CHECK(!parse_version("", f));
        CHECK(!parse_version(nullptr, f));
    }

    CASE("same version, different build: pushed (the 2.12.0-vs-repo case)");
    CHECK(decide(id(2, 12, 8, 111), id(2, 12, 8, 222), 0, false) == Action::Push);

    CASE("a C6 from before build ids reports -1, and gets the image");
    CHECK(decide(id(2, 12, 0, -1), id(2, 12, 8, 222), 0, false) == Action::Push);

    CASE("already running it: nothing");
    CHECK(decide(id(2, 12, 8, 222), id(2, 12, 8, 222), 0, false) == Action::None);

    CASE("an older image than the C6 runs is still pushed: the C6 runs what ships");
    CHECK(decide(id(2, 13, 0, 5), id(2, 12, 8, 222), 0, false) == Action::Push);

    CASE("tried once and it did not take: no restart loop");
    CHECK(decide(id(2, 12, 0, -1), id(2, 12, 8, 222), 222, false) == Action::AlreadyTried);
    CASE("a new image gets a new try");
    CHECK(decide(id(2, 12, 0, -1), id(2, 12, 8, 333), 222, false) == Action::Push);

    CASE("dev may change the build within a major version, not across one");
    CHECK(decide(id(2, 12, 0, -1), id(2, 12, 8, 222), 0, true) == Action::Push);
    CHECK(decide(id(1, 9, 0, -1), id(2, 12, 8, 222), 0, true) == Action::DevRefused);

    CASE("no image, or a C6 that did not answer: leave it alone");
    {
        FwId none;
        CHECK(decide(id(2, 12, 0, -1), none, 0, false) == Action::None);
        CHECK(decide(none, id(2, 12, 8, 222), 0, false) == Action::None);
    }
    return check::report("c6");
}
