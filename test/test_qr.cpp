#include "check.hpp"
#include "core/qr/qr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace core::qr;

namespace {
// Writes the code as a PBM with a 4-module quiet zone, 8 px a module, for
// `zbarimg` (QR_PBM=dir; the decode was checked by hand, see the commit).
void dump(const Code& c, const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    const int q = 4, px = 8, n = (kSize + 2 * q) * px;
    std::fprintf(f, "P1\n%d %d\n", n, n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int mx = x / px - q, my = y / px - q;
            const bool d = mx >= 0 && my >= 0 && mx < kSize && my < kSize && c.dark(mx, my);
            std::fputc(d ? '1' : '0', f);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
}

uint32_t hash(const Code& c) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) h = (h ^ (c.dark(x, y) ? 1u : 0u)) * 16777619u;
    return h;
}
}  // namespace

int main() {
    CASE("the join string phones read");
    {
        char s[64];
        CHECK(wifi_join(s, sizeof s, "CRF250F-Dash", "12345678") == 38);
        CHECK(std::strcmp(s, "WIFI:T:WPA;S:CRF250F-Dash;P:12345678;;") == 0);
    }

    CASE("finders in three corners, the timing lines, the dark module");
    {
        Code c;
        CHECK(encode("WIFI:T:WPA;S:CRF250F-Dash;P:12345678;;", c));
        for (int i = 0; i < 7; ++i) {
            CHECK(c.dark(i, 0) && c.dark(0, i));                    // top left ring
            CHECK(c.dark(kSize - 1 - i, 0) && c.dark(i, kSize - 1));
        }
        CHECK(!c.dark(7, 0) && !c.dark(0, 7));                      // separators
        CHECK(c.dark(3, 3) && !c.dark(1, 1));
        for (int i = 8; i < kSize - 8; ++i) CHECK(c.dark(i, 6) == (i % 2 == 0));
        CHECK(c.dark(8, kSize - 8));
        CHECK(c.dark(22, 22) && !c.dark(21, 22) && c.dark(20, 22)); // alignment
    }

    CASE("the same text gives the same code, and a new password a new one");
    {
        Code a, b;
        CHECK(encode("WIFI:T:WPA;S:CRF250F-Dash;P:12345678;;", a));
        CHECK(encode("WIFI:T:WPA;S:CRF250F-Dash;P:12345679;;", b));
        CHECK(hash(a) != hash(b));
        Code a2;
        encode("WIFI:T:WPA;S:CRF250F-Dash;P:12345678;;", a2);
        CHECK(hash(a) == hash(a2));
        if (const char* dir = std::getenv("QR_PBM")) {
            char path[256];
            std::snprintf(path, sizeof path, "%s/a.pbm", dir);
            dump(a, path);
        }
    }

    CASE("too long is refused and leaves the code alone");
    {
        Code c;
        encode("x", c);
        const uint32_t before = hash(c);
        char big[kMaxBytes + 2];
        std::memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(!encode(big, c));
        CHECK(hash(c) == before);
        big[kMaxBytes] = '\0';
        CHECK(encode(big, c));                                      // exactly full
    }

    // QR_PBM_MANY=dir: one PBM per line of stdin, named by line number.
    if (const char* dir = std::getenv("QR_PBM_MANY")) {
        char line[64], path[256];
        int n = 0;
        while (std::fgets(line, sizeof line, stdin)) {
            line[std::strcspn(line, "\n")] = '\0';
            Code c;
            if (!encode(line, c)) continue;
            std::snprintf(path, sizeof path, "%s/%04d.pbm", dir, n++);
            dump(c, path);
        }
    }
    return check::report("qr");
}
