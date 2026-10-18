// `logic`'s half of saved tracks (core::track), through the real logic_tick
// and the real track_store, against a card that lives in RAM: ride a track,
// turn the key off, come back, and the line, the splits, the record and the
// reference are there without a button pressed.
#include "check.hpp"
#include "services/logic_tick.hpp"
#include "services/track_store.hpp"
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using core::state::CommandId;

class RamCard final : public hal::ICard {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool busy = false;          // every call answers Busy, as a held lock does

    hal::Status init() override { return hal::Status::Ok; }
    bool present() const override { return true; }
    hal::Status append(const char* path, const void* data, size_t len) override {
        if (busy) return hal::Status::Busy;
        auto& f = files[path];
        const auto* p = static_cast<const uint8_t*>(data);
        f.insert(f.end(), p, p + len);
        return hal::Status::Ok;
    }
    hal::Status sync() override { return hal::Status::Ok; }
    hal::Status size_of(const char* path, size_t& out) override {
        if (busy) return hal::Status::Busy;
        auto it = files.find(path);
        if (it == files.end()) return hal::Status::NotPresent;
        out = it->second.size();
        return hal::Status::Ok;
    }
    hal::Status read_at(const char* path, size_t offset, void* buf, size_t len,
                        size_t& got) override {
        got = 0;
        if (busy) return hal::Status::Busy;
        auto it = files.find(path);
        if (it == files.end()) return hal::Status::NotPresent;
        const auto& f = it->second;
        if (offset > f.size()) return hal::Status::IoError;
        got = std::min(len, f.size() - offset);
        std::memcpy(buf, f.data() + offset, got);
        return hal::Status::Ok;
    }
    hal::Status list(const char*, ListFn, void*) override { return hal::Status::Ok; }
    hal::Status remove(const char* path) override {
        if (busy) return hal::Status::Busy;
        return files.erase(path) ? hal::Status::Ok : hal::Status::NotPresent;
    }
    hal::Status write_file(const char* path, const void* data, size_t len) override {
        if (busy) return hal::Status::Busy;
        const auto* p = static_cast<const uint8_t*>(data);
        files[path].assign(p, p + len);
        return hal::Status::Ok;
    }
    hal::Status rename(const char* from, const char* to) override {
        if (busy) return hal::Status::Busy;
        if (files.count(to)) return hal::Status::Busy;
        auto it = files.find(from);
        if (it == files.end()) return hal::Status::NotPresent;
        files[to] = std::move(it->second);
        files.erase(from);
        return hal::Status::Ok;
    }
};

class Script final : public services::ICommandSource {
public:
    std::deque<core::state::Command> q;
    void push(CommandId id) { q.emplace_back(id); }
    bool take(core::state::Command& out) override {
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kLat0 = 40.0;
constexpr double kLon0 = -105.0;

core::geo::LatLon at_ft(double x, double y) {
    return {kLat0 + y / 364000.0,
            kLon0 + x / (364000.0 * std::cos(kLat0 * kPi / 180.0))};
}

// A circle ridden anticlockwise; `start_rad` is where the ride begins, so a
// second visit can start somewhere else and put this power-on's local frame
// somewhere else too.
struct Circle {
    double cx = 0.0, cy = 0.0;
    double r_ft = 487.0;
    double mph = 60.0;
    double start_rad = -kPi / 2 - 0.05;     // just before the south point
    double period_ms() const { return 2.0 * kPi * r_ft / (mph * 5280.0 / 3600.0) * 1000.0; }
    double angle(double t_ms) const { return start_rad + 2.0 * kPi * t_ms / period_ms(); }
    core::geo::LatLon pos(double t_ms) const {
        const double a = angle(t_ms);
        return at_ft(cx + r_ft * std::cos(a), cy + r_ft * std::sin(a));
    }
    // Clockwise from north; anticlockwise motion at angle a heads (-sin, cos).
    float course(double t_ms) const {
        const double a = angle(t_ms);
        double h = std::atan2(-std::sin(a), std::cos(a)) * 180.0 / kPi;
        if (h < 0) h += 360.0;
        return static_cast<float>(h);
    }
};

// One power-on: its own slots, state and mailbox, the card shared.
struct Ride {
    core::state::Slots     slots;
    core::config::Settings settings;
    services::Deps         deps;
    std::unique_ptr<services::LogicState>   state{new services::LogicState};
    std::unique_ptr<services::TrackMailbox> box{new services::TrackMailbox};
    std::unique_ptr<core::delta::Tracker>   delta{new core::delta::Tracker};
    Script                 cmds;
    RamCard&               card;
    uint32_t               t = 0;           // ms into the ride
    uint32_t               next_fix = 0;
    bool                   delta_seen = false;
    int32_t                worst_delta = 0;

    explicit Ride(RamCard& c) : card(c) {
        deps.slots = &slots;
        deps.settings = &settings;
        deps.delta = delta.get();
        deps.tracks = box.get();
    }

    // Rides `c` for `ms`, the fixes at 10 Hz and logic at 50 Hz, `storage`
    // serving the box every 100 ms. A dead receiver sends nothing at all.
    void ride(const Circle& c, uint32_t ms, bool gps = true) {
        constexpr uint32_t kBoot = 5000;
        const uint32_t end = t + ms;
        for (; t < end; t += 20) {
            const uint32_t now = kBoot + t + 3;
            // A fix is seen once it has arrived, 40 ms after it was taken.
            while (gps && kBoot + next_fix + 40 <= now) {
                core::state::Gnss g;
                const core::geo::LatLon p = c.pos(next_fix);
                g.fix = true;
                g.sats = 12;
                g.hdop = 0.9f;
                g.lat = p.lat;
                g.lon = p.lon;
                g.course_deg = c.course(next_fix);
                g.speed_mph = static_cast<float>(c.mph);
                g.year = 2026;
                g.day_of_year = 290;
                g.utc_ms = 36000000 + next_fix;
                g.rx_ms = kBoot + next_fix + 40;
                slots.gnss.publish(g);
                next_fix += 100;
            }
            services::logic_tick(deps, *state, cmds, now);
            if (t % 100 == 0) services::serve_tracks(*box, card);
            const core::state::Lap l = slots.lap.get();
            if (l.delta_valid) {
                delta_seen = true;
                if (std::abs(l.delta_ms) > std::abs(worst_delta)) worst_delta = l.delta_ms;
            }
        }
    }
};

}  // namespace

int main() {
    const Circle track;
    RamCard card;

    CASE("a line set on a first visit is saved with its splits, record and reference");
    {
        Ride r(card);
        r.ride(track, 500);
        CHECK(r.state->tracks_known);              // the list came back empty
        r.cmds.push(CommandId::LapSetLine);
        r.ride(track, static_cast<uint32_t>(4.2 * track.period_ms()));
        const core::state::Lap lap = r.slots.lap.get();
        CHECK(lap.count >= 3);
        CHECK(lap.track == 0);
        CHECK(!lap.track_recalled);
        CHECK(lap.track_best_ms == lap.best_ms);
        CHECK(lap.track_ideal_ms > 0);

        CHECK(card.files.count("tracks/t00.trk") == 1);
        CHECK(card.files.count("tracks/t00.tmp") == 0);
        core::track::Record rec;
        static core::delta::Point pts[core::delta::kMaxPoints];
        size_t n = 0;
        const auto& f = card.files["tracks/t00.trk"];
        CHECK(core::track::decode(f.data(), f.size(), rec, pts, core::delta::kMaxPoints, n));
        CHECK(rec.used && rec.splits);
        CHECK(rec.best_ms == lap.best_ms);
        CHECK(rec.ref_ms == lap.best_ms);
        CHECK(n > 300);
        // Kept relative to the line: the first point is a few feet from it.
        CHECK(std::hypot(pts[0].x_ft, pts[0].y_ft) < 20.0f);
        // And timed: the reference is a lap, start to finish.
        CHECK(pts[0].elapsed_ms < 200);
        CHECK(std::fabs(static_cast<double>(pts[n - 1].elapsed_ms) - rec.ref_ms) < 200.0);
        CHECK(rec.sessions == 1);
        CHECK(rec.last_day != 0);
    }

    const double best_first_visit = [&] {
        core::track::Record rec;
        size_t n = 0;
        const auto& f = card.files["tracks/t00.trk"];
        core::track::decode_header(f.data(), f.size(), rec, n);
        return static_cast<double>(rec.best_ms);
    }();

    CASE("a second visit brings it all back by itself, the delta from the first lap");
    {
        Ride r(card);
        Circle late = track;
        // Starts on the far side, so this power-on's frame is somewhere else
        // and the saved reference has to be moved into it.
        late.start_rad = kPi / 2;
        r.ride(late, 2000);
        core::state::Lap lap = r.slots.lap.get();
        CHECK(lap.track == 0);
        CHECK(lap.track_recalled);
        CHECK(lap.phase == core::lap::Phase::Armed);
        CHECK(lap.track_best_ms == static_cast<uint32_t>(best_first_visit));
        CHECK(r.state->ref_ms != 0);
        CHECK(r.delta->has_reference());
        CHECK(r.state->sectors.placed());

        // Half a lap to the line, then most of a lap: the delta is live on
        // the very first lap, against last visit's best, and near zero
        // because this lap is ridden exactly as that one was.
        r.ride(late, static_cast<uint32_t>(1.4 * late.period_ms()));
        lap = r.slots.lap.get();
        CHECK(lap.phase == core::lap::Phase::Timing);
        CHECK(lap.count == 0);
        CHECK(r.delta_seen);
        std::printf("       (worst delta on lap 1: %d ms)\n", static_cast<int>(r.worst_delta));
        CHECK(std::abs(r.worst_delta) <= 200);
        // Splits from last time: the first lap times sectors already.
        CHECK(lap.sector >= 1);
        r.ride(late, static_cast<uint32_t>(0.5 * late.period_ms()));
        CHECK(r.slots.lap.get().count == 1);

        core::track::Record rec;
        size_t n = 0;
        const auto& f = card.files["tracks/t00.trk"];
        CHECK(core::track::decode_header(f.data(), f.size(), rec, n));
        CHECK(rec.sessions == 2);
        CHECK(n > 300);                // the reference is still in the file
    }

    CASE("a slower visit reads behind the saved best, and grows behind");
    {
        Ride r(card);
        Circle slow = track;
        slow.mph = 54.0;               // a tenth off the pace
        r.ride(slow, 2000);
        CHECK(r.slots.lap.get().track_recalled);
        // Over the line, a whole lap, and a third of the next: a tenth off
        // the pace is ~1.3 s down a third of the way round.
        r.ride(slow, static_cast<uint32_t>(1.3 * slow.period_ms()));
        const core::state::Lap lap = r.slots.lap.get();
        CHECK(lap.delta_valid);
        std::printf("       (delta a third round at 54 mph: %d ms)\n", static_cast<int>(lap.delta_ms));
        CHECK(lap.delta_ms >= 900);
        CHECK(lap.delta_ms <= 1800);
    }

    CASE("Clear laps keeps the track saved but stops it coming straight back");
    {
        Ride r(card);
        r.ride(track, 1000);
        CHECK(r.slots.lap.get().track == 0);
        r.cmds.push(CommandId::LapClear);
        r.ride(track, 3000);
        const core::state::Lap lap = r.slots.lap.get();
        CHECK(lap.phase == core::lap::Phase::NoLine);
        CHECK(lap.track == -1);
        CHECK(card.files.count("tracks/t00.trk") == 1);
    }

    CASE("a line set well away from every saved one is a new track");
    {
        Ride r(card);
        Circle other = track;
        other.cx = 20000.0;
        r.ride(other, 500);
        CHECK(r.slots.lap.get().phase == core::lap::Phase::NoLine);
        r.cmds.push(CommandId::LapSetLine);
        r.ride(other, 1000);
        CHECK(r.slots.lap.get().track == 1);
        CHECK(card.files.count("tracks/t01.trk") == 1);
        CHECK(card.files.count("tracks/t00.trk") == 1);
    }

    CASE("a line set before the card answers is saved where it was set");
    {
        RamCard slow;
        slow.busy = true;
        Ride r(slow);
        Circle third = track;
        third.cx = -30000.0;
        r.ride(third, 300);
        r.cmds.push(CommandId::LapSetLine);
        r.ride(third, 300);
        CHECK(r.state->track_unsaved);
        const core::geo::LatLon set_at = r.state->pending_line.at;
        r.ride(third, 3000);                    // the bike has moved on
        slow.busy = false;
        r.ride(third, 7000);                    // the next try, 5 s on
        CHECK(r.slots.lap.get().track == 0);
        core::track::Record rec;
        size_t n = 0;
        const auto& f = slow.files["tracks/t00.trk"];
        CHECK(core::track::decode_header(f.data(), f.size(), rec, n));
        CHECK(rec.finish.at.lat == set_at.lat);
        CHECK(rec.finish.at.lon == set_at.lon);
    }

    CASE("Forget track deletes it, and the next visit starts from nothing");
    {
        Ride r(card);
        r.ride(track, 1000);
        CHECK(r.slots.lap.get().track == 0);
        r.cmds.push(CommandId::TrackForget);
        r.ride(track, 1000);
        CHECK(r.slots.lap.get().phase == core::lap::Phase::NoLine);
        CHECK(card.files.count("tracks/t00.trk") == 0);
        CHECK(card.files.count("tracks/t01.trk") == 1);

        Ride again(card);
        again.ride(track, 2000);
        CHECK(again.slots.lap.get().phase == core::lap::Phase::NoLine);
        CHECK(again.slots.lap.get().track == -1);
    }

    CASE("no card, no trouble: the lap timer works as it always did");
    {
        Ride r(card);
        r.deps.tracks = nullptr;
        Circle fourth = track;
        fourth.cx = 60000.0;
        r.ride(fourth, 500);
        r.cmds.push(CommandId::LapSetLine);
        r.ride(fourth, static_cast<uint32_t>(3.5 * fourth.period_ms()));
        const core::state::Lap lap = r.slots.lap.get();
        CHECK(lap.count >= 2);
        CHECK(lap.track == -1);
        CHECK(r.delta->has_reference());
    }

    return check::report("logic_track");
}
