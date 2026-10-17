#include "check.hpp"
#include "core/lap/sectors.hpp"
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace core::lap;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLat0 = 40.0;
constexpr double kLon0 = -105.0;
constexpr double kFtPerDegLat = 364000.0;

core::geo::LatLon at_ft(double x, double y) {
    return {kLat0 + y / kFtPerDegLat,
            kLon0 + x / (kFtPerDegLat * std::cos(kLat0 * kPi / 180.0))};
}

// A circle ridden anticlockwise, fixes every 100 ms, the finish line across
// its south point. The speed can be set per lap and per third of the circle,
// so one sector can be ridden slower than the others.
struct Ride {
    double r_ft = 500.0;
    double angle = -kPi / 2 - 0.3;   // start a little before the line
    uint32_t t_ms = 1000;
    int lap = 0;                     // completed crossings of the start
    std::function<double(int lap, int third)> mph = [](int, int) { return 60.0; };

    Timer timer;
    Sectors sectors;
    Config cfg;
    std::vector<Split> splits;
    std::vector<uint32_t> laps;
    bool gap_next = false;

    int third() const {
        // Thirds of the circle from the line, anticlockwise.
        double a = angle + kPi / 2;
        a = std::fmod(a, 2 * kPi);
        if (a < 0) a += 2 * kPi;
        return static_cast<int>(a / (2 * kPi / 3));
    }

    void step() {
        const double v_ftps = mph(lap, third()) * 5280.0 / 3600.0;
        const double before = angle + kPi / 2;
        angle += v_ftps * 0.1 / r_ft;
        const double after = angle + kPi / 2;
        if (std::floor(after / (2 * kPi)) > std::floor(before / (2 * kPi))) ++lap;
        t_ms += 100;
        Fix f;
        f.pos = at_ft(r_ft * std::cos(angle), r_ft * std::sin(angle));
        f.speed_mph = static_cast<float>(mph(lap, third()));
        f.valid = !gap_next;
        f.t_ms = t_ms;
        gap_next = false;
        // Compass course of anticlockwise motion: -angle, in degrees.
        double course = -angle * 180.0 / kPi;
        course = std::fmod(course, 360.0);
        if (course < 0) course += 360.0;
        const Update u = timer.update(f, cfg);
        if (u.event == Event::Completed) laps.push_back(u.lap_ms);
        const Split s = sectors.update(f, static_cast<float>(course), u, cfg);
        if (s.crossed) splits.push_back(s);
    }
    void ride_laps(int n) {
        const int target = lap + n;
        while (lap < target) step();
        // A little past the line, so the last crossing has been seen.
        for (int i = 0; i < 5; ++i) step();
    }
    double sector_ms(double mph_v) const {
        return 2 * kPi * r_ft / 3 / (mph_v * 5280.0 / 3600.0) * 1000.0;
    }
};

}  // namespace

int main() {
    CASE("the first clean lap places two splits a third of the way apart");
    {
        Ride r;
        r.timer.set_line(at_ft(0, -r.r_ft), 90.0);
        r.ride_laps(1);                     // the crossing that starts timing
        CHECK(!r.sectors.placed());
        r.ride_laps(1);                     // the first whole lap
        CHECK(r.sectors.placed());
        // A third and two thirds of the way round from the south point.
        for (int k = 0; k < 2; ++k) {
            const double want = -kPi / 2 + 2 * kPi * (k + 1) / 3;
            const core::geo::LatLon p = r.sectors.line_at(k);
            const double y = (p.lat - kLat0) * kFtPerDegLat;
            const double x = (p.lon - kLon0) * kFtPerDegLat * std::cos(kLat0 * kPi / 180.0);
            const double got = std::atan2(y, x);
            double d = std::fabs(got - want);
            if (d > kPi) d = 2 * kPi - d;
            CHECK(d * r.r_ft < 30.0);       // within a fix or so of the spot
        }
        CHECK(r.splits.empty());            // nothing timed before the lines
    }

    CASE("each sector is timed, and the three add up to the lap");
    {
        Ride r;
        r.timer.set_line(at_ft(0, -r.r_ft), 90.0);
        r.ride_laps(2);                     // start, then the placing lap
        r.splits.clear();
        r.laps.clear();
        r.ride_laps(1);
        CHECK(r.splits.size() == 3);
        CHECK(r.laps.size() == 1);
        if (r.splits.size() == 3 && r.laps.size() == 1) {
            uint32_t sum = 0;
            for (int k = 0; k < 3; ++k) {
                CHECK(r.splits[k].sector == k);
                sum += r.splits[k].ms;
                // A third of the lap give or take where the line fell.
                CHECK(std::fabs(r.splits[k].ms - r.sector_ms(60.0)) < 200.0);
            }
            CHECK(std::abs(static_cast<int>(sum) - static_cast<int>(r.laps[0])) <= 2);
        }
    }

    CASE("a slow sector shows up in its own split, and the ideal takes the best of each");
    {
        // Lap 3 slow in the middle third, lap 4 slow in the last: neither is
        // a perfect lap, but between them every sector was ridden at 60.
        Ride r;
        r.mph = [](int lap, int third) {
            if (lap == 3 && third == 1) return 45.0;
            if (lap == 4 && third == 2) return 45.0;
            if (lap >= 3) return 60.0;
            return 50.0;                    // warm-up laps slower all round
        };
        r.timer.set_line(at_ft(0, -r.r_ft), 90.0);
        r.ride_laps(3);                     // start, place, one timed lap at 50
        r.splits.clear();
        r.ride_laps(1);                     // lap 3
        CHECK(r.splits.size() == 3);
        if (r.splits.size() == 3) {
            CHECK(r.splits[0].delta_valid && r.splits[0].delta_ms < -500);  // 60 vs 50
            CHECK(r.splits[1].delta_valid && r.splits[1].delta_ms > 0);     // 45 vs 50
        }
        r.splits.clear();
        r.ride_laps(1);                     // lap 4
        CHECK(r.splits.size() == 3);
        if (r.splits.size() == 3) {
            CHECK(r.splits[1].delta_valid && r.splits[1].delta_ms < -500);  // 60 vs 50
            CHECK(r.splits[2].delta_valid && r.splits[2].delta_ms > 0);     // 45 vs 50
        }
        const uint32_t ideal = r.sectors.ideal_ms();
        CHECK(std::fabs(ideal - 3 * r.sector_ms(60.0)) < 300.0);
        uint32_t best_lap = 0;
        for (uint32_t l : r.laps) if (best_lap == 0 || l < best_lap) best_lap = l;
        CHECK(ideal < best_lap);            // better than any lap ridden
    }

    CASE("a sector the fix dropped out of is shown but is never a best");
    {
        Ride r;
        r.timer.set_line(at_ft(0, -r.r_ft), 90.0);
        r.ride_laps(3);
        const uint32_t best0 = r.sectors.best_ms(0);
        CHECK(best0 > 0);
        // Ride faster, but lose the fix in the first sector.
        r.mph = [](int, int) { return 70.0; };
        r.splits.clear();
        for (int i = 0; i < 15; ++i) r.step();
        r.gap_next = true;
        r.ride_laps(1);
        CHECK(!r.splits.empty());
        if (!r.splits.empty()) {
            CHECK(r.splits[0].sector == 0);
            CHECK(!r.splits[0].delta_valid);
        }
        CHECK(r.sectors.best_ms(0) == best0);
    }

    CASE("a new finish line forgets the splits; arming keeps them");
    {
        Ride r;
        r.timer.set_line(at_ft(0, -r.r_ft), 90.0);
        r.ride_laps(3);
        CHECK(r.sectors.placed());
        r.timer.arm();
        r.sectors.abort_lap();
        CHECK(r.sectors.placed());
        CHECK(r.sectors.best_ms(1) > 0);
        r.sectors.reset();
        CHECK(!r.sectors.placed());
        CHECK(r.sectors.ideal_ms() == 0);
    }

    CASE("peaks: speed and rpm always, lean only when moving, each way");
    {
        Peaks p;
        p.add(4.0f, 9000.0f, -60.0f);      // revving in the paddock, on its side
        CHECK(p.rpm == 9000.0f);
        CHECK(p.lean_deg() == 0.0f);
        p.add(55.0f, 8000.0f, -41.5f);
        p.add(62.0f, 10500.0f, 37.0f);
        CHECK(p.mph == 62.0f);
        CHECK(p.rpm == 10500.0f);
        CHECK(p.lean_left_deg == 41.5f);
        CHECK(p.lean_right_deg == 37.0f);
        CHECK(p.lean_deg() == 41.5f);
        p.reset();
        CHECK(p.mph == 0.0f && p.lean_deg() == 0.0f);
    }

    return check::report("sectors");
}
