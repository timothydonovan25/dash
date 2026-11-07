#include <cmath>
#include <cstring>
#include <vector>
#include "check.hpp"
#include "core/route/pace.hpp"

using namespace core::route;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int32_t kLat0 = 360000000, kLon0 = -941600000;
const double kMPerLon = 0.011132 * std::cos(36.0 * kPi / 180.0);   // m per 1e-7 deg
constexpr double kMPerLat = 0.011132;

RouteStorage<8192, 16> mem;

// A road drawn by a pen: straights and arcs, a point every 2 m.
struct Pen {
    double x = 0, y = 0, h = 0;          // metres east, north; heading (rad, clockwise from north)
    std::vector<std::pair<double, double>> pts{{0, 0}};
    Pen& straight(double m) {
        for (double d = 2; d <= m + 1e-9; d += 2) pts.emplace_back(x + std::sin(h) * d, y + std::cos(h) * d);
        x += std::sin(h) * m;
        y += std::cos(h) * m;
        return *this;
    }
    // + right. `deg` of turn on radius `r`.
    Pen& arc(double r, double deg) {
        const double len = r * std::fabs(deg) * kPi / 180.0;
        const double sgn = deg > 0 ? 1 : -1;
        for (double d = 2; d <= len + 1e-9; d += 2) {
            h += sgn * 2 / r;
            x += std::sin(h) * 2;
            y += std::cos(h) * 2;
            pts.emplace_back(x, y);
        }
        return *this;
    }
    Route route() {
        Route r = mem.make();
        double dist = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) dist += std::hypot(pts[i].first - pts[i - 1].first, pts[i].second - pts[i - 1].second);
            RoutePoint& p = r.pts[r.n_pts++];
            p = RoutePoint{};
            p.p = Point{kLat0 + int32_t(std::lround(pts[i].second / kMPerLat)),
                        kLon0 + int32_t(std::lround(pts[i].first / kMPerLon))};
            p.dist_dm = uint32_t(std::lround(dist * 10));
        }
        r.cues[r.n_cues++] = Cue{r.n_pts - 1, r.pts[r.n_pts - 1].dist_dm, 0, 0, CueKind::Arrive, 0};
        r.total_dm = r.pts[r.n_pts - 1].dist_dm;
        return r;
    }
};

PaceNote notes[32];

size_t all(const Route& r) { return pace_notes(r, 0, r.total_dm, notes, 32); }

std::string text(const PaceNote& n) {
    char b[32];
    format_note(n, b, sizeof b);
    return b;
}

}  // namespace

int main() {
    CASE("grades: radius to 1..6");
    {
        CHECK(pace_grade(15) == 1);
        CHECK(pace_grade(30) == 2);
        CHECK(pace_grade(50) == 3);
        CHECK(pace_grade(90) == 4);
        CHECK(pace_grade(140) == 5);
        CHECK(pace_grade(220) == 6);
    }

    CASE("a straight road has no notes");
    {
        const Route r = Pen().straight(1000).route();
        CHECK(all(r) == 0);
    }

    CASE("a 90 degree right on 50 m: R3, where it is");
    {
        const Route r = Pen().straight(300).arc(50, 90).straight(300).route();
        CHECK(all(r) == 1);
        CHECK(notes[0].dir == 1);
        CHECK(notes[0].grade == 3);
        CHECK_NEAR(notes[0].deg, 90, 8);
        CHECK_NEAR(notes[0].at_dm, 3000, 250);          // starts about where the arc does
        CHECK(text(notes[0]) == "R3");
    }

    CASE("a left hairpin on 15 m: L1 hairpin");
    {
        const Route r = Pen().straight(300).arc(15, -180).straight(300).route();
        CHECK(all(r) == 1);
        CHECK(notes[0].dir == -1);
        CHECK(notes[0].grade == 1);
        CHECK(notes[0].flags & kPaceHairpin);
        CHECK(text(notes[0]) == "L1 hairpin");
    }

    CASE("an S: right straight into left");
    {
        const Route r = Pen().straight(300).arc(100, 60).arc(100, -60).straight(300).route();
        CHECK(all(r) == 2);
        CHECK(notes[0].dir == 1);
        CHECK(notes[1].dir == -1);
        CHECK(notes[0].grade == 4);
        CHECK(notes[1].grade == 4);
        CHECK(!(notes[0].flags & kPaceInto));
        CHECK(notes[1].flags & kPaceInto);
        CHECK(text(notes[1]) == "into L4");
    }

    CASE("two bends 200 m apart are not into");
    {
        const Route r = Pen().straight(300).arc(60, 45).straight(200).arc(60, -45).straight(300).route();
        CHECK(all(r) == 2);
        CHECK(!(notes[1].flags & kPaceInto));
    }

    CASE("a bend that tightens, and a long one");
    {
        const Route t = Pen().straight(300).arc(150, 50).arc(30, 60).straight(300).route();
        CHECK(all(t) == 1);
        CHECK(notes[0].flags & kPaceTightens);
        CHECK(notes[0].grade == 2);
        const Route l = Pen().straight(300).arc(140, 120).straight(300).route();
        CHECK(all(l) == 1);
        CHECK(notes[0].flags & kPaceLong);
        CHECK(text(notes[0]) == "R5 long");
    }

    CASE("a slight wiggle under 15 degrees is not a note");
    {
        const Route r = Pen().straight(300).arc(200, 10).straight(300).route();
        CHECK(all(r) == 0);
    }

    CASE("a bend at a turn cue is the turn, not a note");
    {
        Route r = Pen().straight(300).arc(30, 90).straight(300).route();
        r.cues[r.n_cues++] = r.cues[0];                 // keep Arrive last
        r.cues[0] = Cue{0, 3240, 0, 90, CueKind::Right, 0};
        CHECK(all(r) == 0);
    }

    CASE("a window: only the bends in it, and one already under way");
    {
        const Route r = Pen().straight(300).arc(50, 90).straight(500).arc(50, -90).straight(500).route();
        CHECK(all(r) == 2);
        const uint32_t second = notes[1].at_dm;
        CHECK(pace_notes(r, second + 100, r.total_dm, notes, 32) == 1);   // inside the second bend
        CHECK(notes[0].dir == -1);
        CHECK(pace_notes(r, 0, 3500, notes, 32) == 1);                      // only the first
        CHECK(notes[0].dir == 1);
        CHECK(pace_notes(r, 0, r.total_dm, notes, 1) == 1);                 // cap
    }

    return check::report("route_pace");
}
