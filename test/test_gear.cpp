#include "check.hpp"
#include "core/gear/gear.hpp"

using namespace core::gear;

namespace {
// Speed that puts the bike squarely in gear `g` at the given rpm.
float speed_for(const Config& c, uint8_t g, float rpm) {
    return rpm / c.rpm_per_mph[g - 1];
}
}  // namespace

int main() {
    const Config cfg;

    CASE("each gear is identified from its own ratio");
    {
        for (uint8_t g = 1; g <= cfg.count; ++g) {
            const float rpm = 6000.0f;
            CHECK(estimate(rpm, speed_for(cfg, g, rpm), false, cfg) == g);
        }
    }

    CASE("the same gear holds across its whole rev range");
    {
        for (float rpm = 1500.0f; rpm <= 9500.0f; rpm += 250.0f) {
            CHECK(estimate(rpm, speed_for(cfg, 3, rpm), false, cfg) == 3);
        }
    }

    CASE("a ratio between two gears picks the nearer one");
    {
        // Third is 215, fourth is 165. Proportionally the crossover sits at
        // about 188, so either side of that goes to the gear it is nearer.
        CHECK(estimate(6000.0f, 6000.0f / 200.0f, false, cfg) == 3);
        CHECK(estimate(6000.0f, 6000.0f / 178.0f, false, cfg) == 4);
    }

    CASE("a ratio off either end of the table clamps to the end gear");
    {
        // No tolerance and no rejection: wheelspin reads as first, and
        // coasting with the engine barely turning reads as top. The tile is
        // a reminder of roughly where you are in the box, not an instrument.
        CHECK(estimate(6000.0f, 6000.0f / 900.0f, false, cfg) == 1);
        CHECK(estimate(6000.0f, 6000.0f / 60.0f, false, cfg) == 5);
    }

    CASE("neutral is sensed, so this does not answer for it");
    {
        CHECK(estimate(6000.0f, speed_for(cfg, 2, 6000.0f), true, cfg)
              == kUnknown);
    }

    CASE("stopped has no ratio to read");
    {
        // Dividing by a road speed of zero is the one thing this cannot do.
        CHECK(estimate(6000.0f, 0.0f, false, cfg) == kUnknown);
        CHECK(estimate(6000.0f, 1.0f, false, cfg) == kUnknown);
        CHECK_NEAR(ratio(6000.0f, 0.0f, cfg), 0.0, 1e-9);
    }

    CASE("an engine that is not turning is not in a gear");
    {
        // Coasting with the motor off: rpm/mph would be near zero and the
        // nearest match would be top gear, which is arithmetic rather than
        // fact.
        CHECK(estimate(0.0f, 30.0f, false, cfg) == kUnknown);
    }

    CASE("the measured ratio is exposed so the table can be calibrated");
    {
        CHECK_NEAR(ratio(6000.0f, 30.0f, cfg), 200.0, 0.01);
    }

    CASE("a table with fewer gears than the maximum is respected");
    {
        Config four = cfg;
        four.count = 4;
        // Fifth's ratio on a four-speed table is nearest to fourth, and that
        // is the right answer for a table that says there is no fifth.
        CHECK(estimate(6000.0f, speed_for(cfg, 5, 6000.0f), false, four) == 4);
    }

    CASE("unconfigured slots are skipped");
    {
        // The sixth ratio is 0 by default, meaning "not fitted". It must not
        // win by being closest to a small ratio.
        Config six = cfg;
        six.count = 6;
        CHECK(estimate(6000.0f, 6000.0f / 120.0f, false, six) == 5);
    }

    CASE("the default ratios fall in order");
    {
        // First is the shortest gear and so has the highest rpm per mph. A
        // table entered out of order would still produce answers, just
        // consistently wrong ones, and nothing else would notice.
        for (uint8_t i = 0; i + 1 < cfg.count; ++i) {
            CHECK(cfg.rpm_per_mph[i] > cfg.rpm_per_mph[i + 1]);
        }
    }

    return check::report("gear");
}
