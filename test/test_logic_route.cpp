// `logic`'s half of routing, end to end: NavGo through the real
// logic_tick, served by the real serve_route from a card in RAM holding a
// map the real builder made, out to Frame::nav. The parts are tested on
// their own in test_route*; this is the wiring between them.
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "rgf_build.hpp"
#include "search_build.hpp"
#include "services/logic_tick.hpp"

using core::route::Phase;
using core::state::CommandId;

namespace {

class Queue final : public services::ICommandSource {
public:
    bool take(core::state::Command& out) override {
        if (q.empty()) return false;
        out = q.front();
        q.pop_front();
        return true;
    }
    std::deque<core::state::Command> q;
};

class RamCard final : public hal::ICard {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    hal::Status init() override { return hal::Status::Ok; }
    bool present() const override { return true; }
    hal::Status append(const char*, const void*, size_t) override { return hal::Status::Unsupported; }
    hal::Status sync() override { return hal::Status::Ok; }
    hal::Status size_of(const char* path, size_t& out) override {
        auto it = files.find(path);
        if (it == files.end()) return hal::Status::NotPresent;
        out = it->second.size();
        return hal::Status::Ok;
    }
    hal::Status read_at(const char* path, size_t off, void* buf, size_t len, size_t& got) override {
        got = 0;
        auto it = files.find(path);
        if (it == files.end()) return hal::Status::NotPresent;
        if (off > it->second.size()) return hal::Status::IoError;
        got = std::min(len, it->second.size() - off);
        std::memcpy(buf, it->second.data() + off, got);
        return hal::Status::Ok;
    }
    hal::Status list(const char*, ListFn, void*, bool) override { return hal::Status::Ok; }
    hal::Status remove(const char*) override { return hal::Status::Unsupported; }
    hal::Status write_file(const char* path, const void* data, size_t len) override {
        const auto* p = static_cast<const uint8_t*>(data);
        files[path].assign(p, p + len);
        return hal::Status::Ok;
    }
    hal::Status rename(const char*, const char*) override { return hal::Status::Unsupported; }
};

// A to B is 4 km east. "US 412" (an untagged divided highway: 65 mph,
// presumed) runs straight there; the slow way goes 0.5 km north up
// "Oak St", along "Ridge Rd" and back down "Elm St".
constexpr double kLatA = 36.0, kLonA = -94.2;
constexpr double kKmLon = 1.0 / (111.195 * 0.80902);   // degrees of longitude per km at 36 N
constexpr double kKmLat = 1.0 / 111.195;

std::vector<uint8_t> town() {
    routegraph::Rgi g;
    g.osm_date = 20261102;
    g.names = {'\0'};
    auto name = [&](const char* s) {
        const uint32_t o = uint32_t(g.names.size());
        g.names.insert(g.names.end(), s, s + std::strlen(s) + 1);
        return o;
    };
    auto node = [&](double lat, double lon) {
        g.nodes.push_back({int32_t(std::lround(lat * 1e7)), int32_t(std::lround(lon * 1e7))});
        return uint32_t(g.nodes.size() - 1);
    };
    const uint32_t a = node(kLatA, kLonA), b = node(kLatA, kLonA + 4 * kKmLon);
    const uint32_t c = node(kLatA + 0.5 * kKmLat, kLonA), d = node(kLatA + 0.5 * kKmLat, kLonA + 4 * kKmLon);
    auto road = [&](uint32_t u, uint32_t v, uint32_t len_m, uint8_t cls, uint32_t nm) {
        routegraph::RgiEdge e{};
        e.u = u; e.v = v; e.length_dm = len_m * 10; e.cls = cls; e.name_off = nm;
        g.edges.push_back(e);
    };
    road(a, b, 4000, 1, name("US 412"));          // trunk
    road(a, c, 500, 6, name("Oak St"));
    road(c, d, 4000, 6, name("Ridge Rd"));
    road(d, b, 500, 6, name("Elm St"));
    return routegraph::build_rgf(g, routegraph::Options{});
}

struct Ride {
    core::state::Slots slots;
    core::config::Settings settings;
    services::Deps deps;
    std::unique_ptr<services::LogicState> s{new services::LogicState};
    std::unique_ptr<services::RouteMailbox> box{new services::RouteMailbox};
    std::unique_ptr<services::RouteEngine> eng{new services::RouteEngine};
    std::unique_ptr<services::RouteTrace> trace{new services::RouteTrace};
    RamCard card;
    Queue q;
    uint32_t now = 1000;

    Ride() {
        deps.slots = &slots;
        deps.settings = &settings;
        deps.route = box.get();
        deps.route_engine = eng.get();
        deps.route_trace = trace.get();
    }
    void at(double lat, double lon) {
        core::state::Gnss g;
        g.fix = true;
        g.hdop = 1.0f;
        g.lat = lat;
        g.lon = lon;
        g.year = 2026;
        g.day_of_year = 307;
        g.utc_ms = now;
        g.rx_ms = now;
        slots.gnss.publish(g);
    }
    // logic, then storage, as the two tasks would interleave.
    void run(int passes = 1) {
        for (int i = 0; i < passes; ++i) {
            services::logic_tick(deps, *s, q, now);
            services::serve_route(*box, *eng, card);
            now += 20;
        }
    }
    const core::state::Nav nav() const { return slots.nav.get(); }
    int find(const char* name) const {
        const core::state::Nav n = nav();
        for (int i = 0; i < n.n_dest; ++i)
            if (std::strcmp(n.dest[i].name, name) == 0) return i;
        return -1;
    }
};

}  // namespace

int main() {
    CASE("no map on the card: NAV says so, and keeps looking");
    {
        Ride r;
        r.at(kLatA, kLonA);
        r.run(5);
        CHECK(r.nav().phase == uint8_t(Phase::NoMap));
        CHECK(r.nav().map_date == 0);
        r.card.files[services::kMapPath] = town();
        r.now += 30000;                                // the next look
        r.run(5);
        CHECK(r.nav().phase == uint8_t(Phase::Idle));
        CHECK(r.nav().map_date == 20261102);
    }

    auto ride = std::make_unique<Ride>();
    Ride& r = *ride;
    r.card.files[services::kMapPath] = town();
    const std::string csv = "Ferry,36.0," + std::to_string(kLonA + 4 * kKmLon) + "\n";
    r.card.files[services::kDestPath].assign(csv.begin(), csv.end());
    r.at(kLatA, kLonA);
    r.run(10);

    CASE("the picker: START set by the first fix, then the card's favourite");
    {
        const core::state::Nav n = r.nav();
        CHECK(n.phase == uint8_t(Phase::Idle));
        CHECK(n.n_dest == 2);
        CHECK(std::string(n.dest[0].name) == "START");
        CHECK(n.dest[0].kind == core::state::NavDest::Waypoint);
        CHECK(r.find("Ferry") == 1);
    }

    CASE("the speed limit setting reaches the navigator: 55");
    {
        CHECK(r.nav().max_mph == 0);
        r.q.q.push_back(core::state::Command::config_set(core::config::kKeyNavSpeedLimit, 2.0f));
        r.run(2);
        CHECK(r.settings.nav_speed_limit == 2);
        CHECK(r.nav().max_mph == 55);
        CHECK(r.s->nav.limit() == core::route::limit_flags(55));
    }

    CASE("go under 55: US 412 (an untagged divided highway, so 65) is left out, nothing asked");
    {
        r.q.q.emplace_back(CommandId::NavGo, int32_t{r.find("Ferry")});
        r.run(6);
        const core::state::Nav n = r.nav();
        CHECK(n.phase == uint8_t(Phase::Guiding));
        CHECK(n.following);
        CHECK(std::string(n.target) == "Ferry");
        CHECK(!n.over_limit);
        CHECK_NEAR(n.remain_mi, 5.0 / 1.609344, 0.05);
        CHECK(std::string(n.cue_road) == "Ridge Rd");  // first turn: right onto Ridge Rd
        CHECK(n.cue_kind == uint8_t(core::route::CueKind::Right));
        CHECK_NEAR(n.to_cue_ft, 500 * 3.28084, 30);
    }

    CASE("the MAP screen's copy of the route: filled at each new route, ends where it ends");
    {
        const services::RouteTrace& t = *r.trace;
        CHECK(t.gen.load() >= 1);
        CHECK(t.pts.size() >= 4);
        core::state::TrackPoint first, last;
        CHECK(t.pts.read_nth(0, first));
        CHECK(t.pts.read_nth(t.pts.size() - 1, last));
        // In feet from the session origin (the first fix, at A): from A to
        // B, 4 km east, by way of Ridge Rd 0.5 km north.
        CHECK_NEAR(first.x_ft, 0.0, 30);
        CHECK_NEAR(last.x_ft, 4000 * 3.28084, 60);
        float max_y = 0;
        for (size_t i = 0; i < t.pts.size(); ++i) {
            core::state::TrackPoint p;
            if (t.pts.read_nth(i, p) && p.y_ft > max_y) max_y = p.y_ft;
        }
        CHECK_NEAR(max_y, 500 * 3.28084, 60);
    }

    CASE("riding it: the next cue moves on and the miles come down");
    {
        r.at(kLatA + 0.5 * kKmLat, kLonA + 2 * kKmLon);     // halfway along Ridge Rd
        r.run(2);
        const core::state::Nav n = r.nav();
        CHECK(std::string(n.cue_road) == "Elm St");
        CHECK(n.cue_kind == uint8_t(core::route::CueKind::Right));
        CHECK_NEAR(n.remain_mi, 2.5 / 1.609344, 0.05);
        CHECK(!n.off_route);
    }

    CASE("off the route onto the highway: rerouted along it, drawn amber");
    {
        // Under 55, the bike wanders 500 m south onto US 412, out of reach
        // of any slower road, so there is no route under the limit from
        // here: the reroute may use US 412 (the straight way to B).
        const uint32_t gen0 = r.trace->gen.load();
        r.at(kLatA, kLonA + 2 * kKmLon);
        r.run(2);
        CHECK(r.nav().off_route);
        CHECK(r.nav().phase == uint8_t(Phase::Guiding));    // not yet: 5 s off first
        r.now += 10000;            // 5 s off, and 10 s since the last route asked for
        r.at(kLatA, kLonA + 2 * kKmLon);
        r.run(12);
        r.now += 1000;                                      // the next fix
        r.at(kLatA, kLonA + 2 * kKmLon);
        r.run(2);
        const core::state::Nav n = r.nav();
        CHECK(n.phase == uint8_t(Phase::Guiding));
        CHECK(n.following);
        CHECK(!n.off_route);
        CHECK(n.over_limit);
        CHECK_NEAR(n.remain_mi, 2.0 / 1.609344, 0.05);
        CHECK(r.trace->gen.load() > gen0);                  // a new route on the MAP screen
        // The route view shows the highway amber: 65 on its points, all 2 km.
        float fast_ft = 0, x0 = 0;
        for (size_t i = 0; i < r.trace->pts.size(); ++i) {
            core::state::TrackPoint p;
            if (!r.trace->pts.read_nth(i, p)) continue;
            if (i > 0 && p.speed_mph == 65.0f) fast_ft += p.x_ft - x0;
            x0 = p.x_ft;
        }
        CHECK_NEAR(fast_ft, 2000 * 3.28084, 60);
    }

    CASE("arriving");
    {
        r.at(kLatA + 0.01 * kKmLat, kLonA + 4 * kKmLon);
        r.run(2);
        CHECK(r.nav().phase == uint8_t(Phase::Arrived));
    }

    CASE("stop: back to the picker, nothing followed");
    {
        r.q.q.emplace_back(CommandId::NavCancel);
        r.run(2);
        CHECK(r.nav().phase == uint8_t(Phase::Idle));
        CHECK(!r.nav().following);
        CHECK(r.box->live == 0xFF);
        CHECK(r.trace->pts.size() == 0);           // and off the map
    }

    CASE("a destination from the phone leads the picker and is routed to at once");
    {
        core::state::PhoneDest pd;
        pd.seq = 1;
        std::snprintf(pd.name, sizeof pd.name, "Pub");
        pd.lat_e7 = int32_t(std::lround(kLatA * 1e7));
        pd.lon_e7 = int32_t(std::lround((kLonA + 4 * kKmLon) * 1e7));
        r.at(kLatA, kLonA);
        r.slots.phone_dest.publish(pd);
        r.run(6);
        const core::state::Nav n = r.nav();
        CHECK(n.n_dest >= 1);
        CHECK(std::string(n.dest[0].name) == "Pub");
        CHECK(n.dest[0].kind == core::state::NavDest::Phone);
        CHECK(std::string(n.target) == "Pub");
        CHECK(n.phase == uint8_t(Phase::Guiding));
        // The same one again is not a new request.
        r.q.q.emplace_back(CommandId::NavCancel);
        r.run(2);
        r.slots.phone_dest.publish(pd);
        r.run(3);
        CHECK(r.nav().phase == uint8_t(Phase::Idle));
    }

    CASE("limit off: guided along the highway itself, and it is not over anything");
    {
        r.q.q.push_back(core::state::Command::config_set(core::config::kKeyNavSpeedLimit, 0.0f));
        r.run(2);
        r.at(kLatA, kLonA);
        r.q.q.emplace_back(CommandId::NavGo, int32_t{r.find("Ferry")});
        r.run(6);
        const core::state::Nav n = r.nav();
        CHECK(n.phase == uint8_t(Phase::Guiding));
        CHECK(n.max_mph == 0);
        CHECK(!n.over_limit);
        CHECK_NEAR(n.remain_mi, 4.0 / 1.609344, 0.05);
    }

    CASE("the dirt setting reaches the navigator and the next route");
    {
        CHECK(!r.nav().avoid_dirt);
        r.q.q.push_back(core::state::Command::config_set(core::config::kKeyNavAvoidDirt, 1.0f));
        r.run(2);
        CHECK(r.settings.nav_avoid_dirt);
        CHECK(r.nav().avoid_dirt);
    }

    CASE("Where to?: spell it, pick it, go; a street takes a house number");
    {
        r.q.q.emplace_back(CommandId::NavCancel);
        r.run(2);
        // The search index: the ferry landing, and Elm St with numbers
        // 100 (its south end) to 900 (its north end).
        std::vector<routegraph::RawPlace> v;
        routegraph::RawPlace ferry;
        ferry.kind = core::route::Place::Poi;
        ferry.name = "Ferry Landing";
        ferry.city = "Ozark";
        ferry.p = {int32_t(std::lround(kLatA * 1e7)), int32_t(std::lround((kLonA + 4 * kKmLon) * 1e7))};
        v.push_back(ferry);
        routegraph::RawPlace elm;
        elm.kind = core::route::Place::Street;
        elm.name = "Elm Street";
        elm.city = "Ozark";
        const int32_t elm_lon = int32_t(std::lround((kLonA + 4 * kKmLon) * 1e7));
        elm.p = {int32_t(std::lround((kLatA + 0.25 * kKmLat) * 1e7)), elm_lon};
        elm.nums = {{100, int32_t(std::lround((kLatA + 0.05 * kKmLat) * 1e7)), elm_lon},
                    {900, int32_t(std::lround((kLatA + 0.45 * kKmLat) * 1e7)), elm_lon}};
        v.push_back(elm);
        const std::vector<uint8_t> idx = routegraph::build_search(v, 20261105);
        r.card.files[services::kSearchPath].assign(idx.begin(), idx.end());

        r.at(kLatA, kLonA);
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        r.run(3);
        core::state::Nav n = r.nav();
        CHECK(n.search == core::state::Nav::SearchTyping);
        CHECK(n.search_ok);
        const auto bit = [](char c) { return uint64_t(1) << (std::strchr(core::route::kAlphabet, c) - core::route::kAlphabet); };
        CHECK((n.next & bit('F')) && (n.next & bit('E')) && !(n.next & bit('Q')));
        for (char c : std::string("FER")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(4);
        n = r.nav();
        CHECK(std::string(n.query) == "FER");
        CHECK(n.n_hits == 1);
        CHECK(std::string(n.hits[0].name) == "Ferry Landing");
        CHECK(std::string(n.hits[0].city) == "Ozark");
        CHECK_NEAR(n.hits[0].dist_mi, 4.0 / 1.609344, 0.05);
        // A typo (the screen would not offer it), then DEL.
        r.q.q.emplace_back(CommandId::NavSearchKey, int32_t('Q'));     // a letter that matches nothing
        r.q.q.emplace_back(CommandId::NavSearchKey, 8);
        r.run(4);
        CHECK(std::string(r.nav().query) == "FER");
        // Pick: a place, so straight to routing, and the search closes.
        r.q.q.emplace_back(CommandId::NavSearchPick, 0);
        r.run(6);
        n = r.nav();
        CHECK(n.search == core::state::Nav::SearchOff);
        CHECK(std::string(n.target) == "Ferry Landing");
        CHECK(n.phase == uint8_t(Phase::Guiding));

        // A street: pick it, type 900, GO: routed to the north end of Elm.
        r.q.q.emplace_back(CommandId::NavCancel);
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("ELM")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(6);
        CHECK(r.nav().n_hits == 1 && r.nav().hits[0].numbers);
        r.q.q.emplace_back(CommandId::NavSearchPick, 0);
        r.run(1);
        CHECK(r.nav().search == core::state::Nav::SearchNumber);
        CHECK(std::string(r.nav().street) == "Elm St");     // as signed
        for (char c : std::string("0900")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(1);
        CHECK(std::string(r.nav().digits) == "900");        // no leading 0
        r.q.q.emplace_back(CommandId::NavSearchGo);
        r.run(8);
        n = r.nav();
        CHECK(n.search == core::state::Nav::SearchOff);
        CHECK(std::string(n.target) == "900 Elm St");
        // The route ends at 900, 0.45 km up Elm from the highway: the slow
        // way round (Oak, Ridge, Elm south) or the highway then Elm north.
        CHECK(n.following);
        // Both went into the card's recent list, newest first, and the
        // picker shows them.
        r.run(4);
        CHECK(r.card.files.count(services::kRecentPath) == 1);
        {
            const auto& f = r.card.files[services::kRecentPath];
            const std::string text(f.begin(), f.end());
            CHECK(text.rfind("900 Elm St,", 0) == 0);
            CHECK(text.find("Ferry Landing,") != std::string::npos);
        }
        r.q.q.emplace_back(CommandId::NavCancel);
        r.run(2);
        r.now += 61000;                       // the next destination refresh
        r.run(4);
        CHECK(r.find("900 Elm St") >= 0);
        CHECK(r.nav().dest[r.find("900 Elm St")].kind == core::state::NavDest::Recent);
        // The number first, as a Garmin takes it: "900 ELM" lists that
        // house on Elm St, and picking it goes straight there.
        r.q.q.emplace_back(CommandId::NavCancel);
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("900 ELM")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(10);
        n = r.nav();
        CHECK(n.n_hits == 1);
        CHECK(std::string(n.hits[0].name) == "900 Elm St");
        CHECK(!n.hits[0].numbers);
        // No space needed: a letter after the number puts it in.
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("900ELM")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(10);
        CHECK(std::string(r.nav().query) == "900 ELM");
        CHECK(r.nav().n_hits == 1 && std::string(r.nav().hits[0].name) == "900 Elm St");
        // After 3 digits the street's first letters are on offer too.
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("900")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(6);
        CHECK((r.nav().next & bit('E')) && (r.nav().next & bit('F')) && (r.nav().next & bit(' ')));
        // While the number is being typed, digits and a space stay on offer.
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("13")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(6);
        CHECK((r.nav().next & bit('5')) && (r.nav().next & bit(' ')));
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("900 ELM")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(10);
        r.q.q.emplace_back(CommandId::NavSearchPick, 0);
        r.run(8);
        n = r.nav();
        CHECK(n.search == core::state::Nav::SearchOff);
        CHECK(std::string(n.target) == "900 Elm St");
        CHECK(n.following);
        // Without numbers typed, GO is the street itself.
        r.q.q.emplace_back(CommandId::NavCancel);
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        for (char c : std::string("ELM")) r.q.q.emplace_back(CommandId::NavSearchKey, int32_t(c));
        r.run(6);
        r.q.q.emplace_back(CommandId::NavSearchPick, 0);
        r.q.q.emplace_back(CommandId::NavSearchGo);
        r.run(6);
        CHECK(std::string(r.nav().target) == "Elm St");
        // DEL past the number goes back to the street spelling; hold E closes.
        r.q.q.emplace_back(CommandId::NavCancel);
        r.q.q.emplace_back(CommandId::NavSearch, 1);
        r.q.q.emplace_back(CommandId::NavSearchKey, int32_t('E'));
        r.run(4);
        r.q.q.emplace_back(CommandId::NavSearch, 0);
        r.run(2);
        CHECK(r.nav().search == core::state::Nav::SearchOff);
    }

    CASE("USB drive: parked only, and a refusal is counted for the screen");
    {
        Ride u;
        u.at(kLatA, kLonA);
        u.run(2);
        const uint32_t gen0 = u.slots.storage_request.generation();
        // Parked, with a GPS whose drift reads 4 mph from a poor fix: not moving.
        core::state::Gnss g = u.slots.gnss.get();
        g.speed_mph = 4.0f;
        g.hdop = 6.0f;
        u.slots.gnss.publish(g);
        u.q.q.emplace_back(CommandId::StorageToHost);
        u.run(1);
        CHECK(u.slots.storage_request.generation() != gen0);
        CHECK(u.slots.storage_request.get().op == core::state::StorageRequest::UsbDrive);
        CHECK(u.nav().usb_refused_seq == 0);
        // Riding at 20 mph, wheels and a good fix agreeing: refused, and counted.
        g.speed_mph = 20.0f;
        g.hdop = 0.9f;
        u.slots.gnss.publish(g);
        core::state::Motion m;
        m.front_mph = m.rear_mph = m.best_mph = 20.0f;
        u.slots.motion.publish(m);
        u.run(50);                                   // the fused speed settles
        const uint32_t gen1 = u.slots.storage_request.generation();
        u.q.q.emplace_back(CommandId::StorageToHost);
        u.run(1);
        CHECK(u.slots.storage_request.generation() == gen1);
        CHECK(u.nav().usb_refused_seq == 1);
    }

    return check::report("logic_route");
}
