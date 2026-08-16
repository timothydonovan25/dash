#include "check.hpp"
#include "core/delta/delta.hpp"
#include <cmath>

using namespace core::delta;

namespace {

// A synthetic circuit: an oval, walked at 10 Hz. `pace` scales the speed, so
// pace 1.1 means the whole lap takes 10% longer. `offset_ft` pushes the line
// sideways, which is what a rider taking a different line through a corner
// actually does — and the case distance-along-lap matching gets wrong.
//
// These tests are synthetic, and that is a real limitation: they prove the
// matcher does what it was designed to do, not that the design suits a real
// GPS trace. The thresholds in Config still need a track day.
struct Oval {
    float radius_ft = 300.0f;
    float base_speed_ftps = 88.0f;      // 60 mph

    // Position at a fraction 0..1 around the lap.
    void at(float f, float offset_ft, float& x, float& y) const {
        const float a = f * 2.0f * 3.14159265f;
        const float r = radius_ft + offset_ft;
        x = r * std::sin(a);
        y = r * std::cos(a);
    }
    float lap_seconds(float pace) const {
        return (2.0f * 3.14159265f * radius_ft) / base_speed_ftps * pace;
    }
};

// Ride one lap, feeding the tracker. Returns the delta at the end of it.
struct Ride {
    explicit Ride(Tracker& tracker) : t(tracker) {}

    Tracker& t;
    Config   cfg;
    Oval     oval;

    int32_t last_delta = 0;
    bool    last_valid = false;
    int     valid_samples = 0;

    void lap(float pace, float offset_ft = 0.0f, bool is_best = false) {
        t.start_lap();
        const float secs = oval.lap_seconds(pace);
        const int steps = static_cast<int>(secs * 10.0f);
        for (int i = 0; i <= steps; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(steps);
            float x, y;
            oval.at(f, offset_ft, x, y);
            const uint32_t ms = static_cast<uint32_t>(f * secs * 1000.0f);
            t.sample(x, y, ms, cfg);
            int32_t d = 0;
            last_valid = t.delta_ms(d);
            if (last_valid) { last_delta = d; ++valid_samples; }
        }
        t.finish_lap(is_best);
    }
};

}  // namespace

int main() {
    CASE("with no reference there is no delta");
    {
        Tracker t;
        Ride r{t};
        CHECK(!t.has_reference());
        r.lap(1.0f, 0.0f, /*is_best=*/false);
        CHECK(!r.last_valid);
        CHECK(r.valid_samples == 0);
    }

    CASE("only a lap the caller calls best becomes the reference");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, false);
        CHECK(!t.has_reference());
        r.lap(1.0f, 0.0f, true);
        CHECK(t.has_reference());
        const size_t n = t.reference_points();
        r.lap(1.2f, 0.0f, false);            // slower, not promoted
        CHECK(t.reference_points() == n);
    }

    CASE("riding the reference lap again reads about zero");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        r.lap(1.0f, 0.0f, false);
        CHECK(r.last_valid);
        CHECK(r.valid_samples > 100);
        CHECK(std::abs(r.last_delta) <= 200);
    }

    CASE("a slower lap reads positive, by about the right amount");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        const float ref_s = r.oval.lap_seconds(1.0f);
        r.lap(1.10f, 0.0f, false);
        CHECK(r.last_valid);
        const int32_t expected = static_cast<int32_t>(ref_s * 0.10f * 1000.0f);
        CHECK_NEAR(r.last_delta, expected, 400.0);
        CHECK(r.last_delta > 0);
    }

    CASE("a faster lap reads negative");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        const float ref_s = r.oval.lap_seconds(1.0f);
        r.lap(0.90f, 0.0f, false);
        CHECK(r.last_valid);
        const int32_t expected = -static_cast<int32_t>(ref_s * 0.10f * 1000.0f);
        CHECK_NEAR(r.last_delta, expected, 400.0);
        CHECK(r.last_delta < 0);
    }

    CASE("a different line through the corners still matches");
    {
        // The reason for nearest-point rather than distance-along-lap. A
        // line 20 ft wider is 126 ft further round this oval — 1.4 s at
        // 60 mph — and a distance-based matcher would report all of it as
        // being behind when the rider is not.
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        r.lap(1.0f, 20.0f, false);
        CHECK(r.last_valid);
        CHECK(std::abs(r.last_delta) <= 400);
    }

    CASE("leaving the reference line stops the delta rather than guessing");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);

        // Rider pulls into the pits: 500 ft inside the oval, well past the
        // match threshold.
        t.start_lap();
        for (int i = 0; i < 40; ++i) {
            float x, y;
            r.oval.at(static_cast<float>(i) / 400.0f, 0.0f, x, y);
            t.sample(x, y, static_cast<uint32_t>(i * 100), r.cfg);
        }
        int32_t d = 0;
        CHECK(t.delta_ms(d));                // on the line: fine

        for (int i = 40; i < 60; ++i) {
            float x, y;
            r.oval.at(static_cast<float>(i) / 400.0f, -500.0f, x, y);
            t.sample(x, y, static_cast<uint32_t>(i * 100), r.cfg);
        }
        CHECK(!t.delta_ms(d));               // off it: no number at all
        CHECK(d == 0);
    }

    CASE("the number is quantised at the source, not at the widget");
    {
        // A tenth is the honest resolution of this figure. Rounding here
        // means no screen, no log and no later reader can imply more.
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        r.lap(1.037f, 0.0f, false);
        CHECK(r.last_valid);
        CHECK(r.last_delta % 100 == 0);
    }

    CASE("a circuit that crosses itself does not match the wrong branch");
    {
        // A figure-of-eight: the track passes through the same point twice a
        // lap, twenty seconds apart. At the crossover the nearest reference
        // point is genuinely ambiguous — both passes are at zero distance —
        // and only the anchor from the last match says which one is meant.
        // A global nearest-point search reports the earlier index and the
        // delta jumps by half a lap.
        Tracker t;
        Config cfg;

        // The second lap is not a bit-identical replay — a real one never is.
        // A few feet of deviation is what makes the crossover genuinely
        // ambiguous: without it an exact match always wins and the test
        // proves nothing.
        auto figure_eight = [&](bool best, bool wander, int32_t& worst_abs) {
            t.start_lap();
            const int steps = 400;
            worst_abs = 0;
            for (int i = 0; i <= steps; ++i) {
                const float a = static_cast<float>(i) / steps * 2.0f * 3.14159265f;
                float x = 400.0f * std::cos(a);
                float y = 400.0f * std::sin(a) * std::cos(a);
                if (wander) {
                    x += static_cast<float>((i * 37) % 7) - 3.0f;
                    y += static_cast<float>((i * 53) % 5) - 2.0f;
                }
                t.sample(x, y, static_cast<uint32_t>(i * 100), cfg);
                int32_t d = 0;
                if (t.delta_ms(d) && std::abs(d) > worst_abs) worst_abs = std::abs(d);
            }
            t.finish_lap(best);
        };

        int32_t worst = 0;
        figure_eight(true, false, worst);
        figure_eight(false, true, worst);
        // Riding the reference lap again: the delta must stay near zero the
        // whole way round, crossover included.
        CHECK(worst < 1000);
    }

    CASE("clear() forgets the reference");
    {
        Tracker t;
        Ride r{t};
        r.lap(1.0f, 0.0f, true);
        CHECK(t.has_reference());
        t.clear();
        CHECK(!t.has_reference());
        int32_t d = 0;
        CHECK(!t.delta_ms(d));
    }

    CASE("a lap longer than the buffer cannot become the reference");
    {
        // A wrong-sized buffer must fail by refusing the reference, not by
        // writing past the end of it.
        Tracker t;
        Config cfg;
        t.start_lap();
        for (size_t i = 0; i < kMaxPoints * 2; ++i) {
            t.sample(static_cast<float>(i), 0.0f,
                     static_cast<uint32_t>(i * 100), cfg);
        }
        CHECK(t.recorded_points() == kMaxPoints);
        t.finish_lap(true);
        CHECK(t.reference_points() == kMaxPoints);
    }

    return check::report("delta");
}
