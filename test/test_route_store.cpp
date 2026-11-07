// `storage`'s half of routing (services/route_store.hpp): the mailbox
// served from a card in RAM holding a map the real builder made.
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "rgf_build.hpp"
#include "services/route_store.hpp"

using namespace core::route;
using services::RouteMailbox;

namespace {

class RamCard final : public hal::ICard {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    int reads = 0;
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
        ++reads;
        got = 0;
        auto it = files.find(path);
        if (it == files.end()) return hal::Status::NotPresent;
        if (off > it->second.size()) return hal::Status::IoError;
        got = std::min(len, it->second.size() - off);
        std::memcpy(buf, it->second.data() + off, got);
        return hal::Status::Ok;
    }
    hal::Status list(const char*, ListFn, void*, bool) override { return hal::Status::Ok; }
    hal::Status remove(const char* path) override { files.erase(path); return hal::Status::Ok; }
    hal::Status write_file(const char*, const void*, size_t) override { return hal::Status::Unsupported; }
    hal::Status rename(const char*, const char*) override { return hal::Status::Unsupported; }
};

// A straight main street with a side road, near Fayetteville.
routegraph::Rgi town(uint32_t date) {
    routegraph::Rgi g;
    g.osm_date = date;
    g.names = {'\0'};
    auto name = [&](const char* s) {
        const uint32_t o = uint32_t(g.names.size());
        g.names.insert(g.names.end(), s, s + std::strlen(s) + 1);
        return o;
    };
    const uint32_t main_st = name("Main St"), side = name("College Ave"), shell = name("Shell");
    for (int i = 0; i < 4; ++i) g.nodes.push_back({360000000, -941600000 + i * 22300});
    g.nodes.push_back({360000000 + 18000, -941600000 + 2 * 22300});
    for (uint32_t i = 0; i < 3; ++i) {
        routegraph::RgiEdge e{};
        e.u = i; e.v = i + 1; e.length_dm = 2000; e.name_off = main_st; e.cls = 6;
        g.edges.push_back(e);
    }
    routegraph::RgiEdge e{};
    e.u = 2; e.v = 4; e.length_dm = 2000; e.name_off = side; e.cls = 6;
    g.edges.push_back(e);
    g.pois.push_back({360000000 + 18000, -941600000 + 2 * 22300, PoiFuel, {0, 0, 0}, shell});
    g.pois.push_back({360000000, -941600000, PoiFood, {0, 0, 0}, 0});
    return g;
}

std::vector<uint8_t> map_bytes(uint32_t date) {
    return routegraph::build_rgf(town(date), routegraph::Options{});
}

void serve(RouteMailbox& box, services::RouteEngine& eng, RamCard& card, RouteMailbox::Op op) {
    box.op = op;
    box.post();
    CHECK(services::serve_route(box, eng, card));
    CHECK(box.answered());
    box.release();
}

}  // namespace

int main() {
    auto box = std::make_unique<RouteMailbox>();
    auto eng = std::make_unique<services::RouteEngine>();
    RamCard card;

    CASE("nothing posted, nothing done");
    CHECK(!services::serve_route(*box, *eng, card));

    CASE("no map on the card: Open says so, a query is NoMap");
    serve(*box, *eng, card, RouteMailbox::Op::Open);
    CHECK(!box->map_ok);
    CHECK(box->map_date == 0);
    box->req = Request{{360000000, -941600000}, {360000000, -941600000 + 3 * 22300}, 0};
    serve(*box, *eng, card, RouteMailbox::Op::Query);
    CHECK(box->status == Status::NoMap);

    CASE("a map copied on is found by the next job");
    card.files[services::kMapPath] = map_bytes(20261102);
    serve(*box, *eng, card, RouteMailbox::Op::Open);
    CHECK(box->map_ok);
    CHECK(box->map_date == 20261102);

    CASE("a query fills the buffer it was told to, with the roads named");
    box->into = 1;
    box->req = Request{{360000000, -941600000 + 2000}, {360000000 + 15000, -941600000 + 2 * 22300}, 0};
    serve(*box, *eng, card, RouteMailbox::Op::Query);
    CHECK(box->status == Status::Ok);
    const Route& r = box->data[1].route;
    CHECK(r.n_pts >= 3);
    CHECK(box->data[0].route.n_pts == 0);          // the other buffer untouched
    CHECK(r.n_cues == 2);                          // left onto College Ave, then arrive
    CHECK(r.cues[0].kind == CueKind::Left);
    CHECK(std::string(box->data[1].cue_names[0]) == "College Ave");
    CHECK(r.cues[1].kind == CueKind::Arrive);
    CHECK(box->stats.settled > 0);

    CASE("favourites first, then the nearest fuel and food, named from the map");
    card.files[services::kDestPath] = {};
    {
        const std::string csv = "# mine\nHome,36.05,-94.17\nCabin,35.9,-93.9\n";
        card.files[services::kDestPath].assign(csv.begin(), csv.end());
    }
    box->near = Point{360000000, -941600000};
    serve(*box, *eng, card, RouteMailbox::Op::Destinations);
    CHECK(box->n_dests == 4);
    CHECK(std::string(box->dests[0].name) == "Home");
    CHECK(std::string(box->dests[1].name) == "Cabin");
    CHECK(std::string(box->dests[2].name) == "Shell");
    CHECK(box->dests[2].kind == PoiFuel);
    CHECK(std::string(box->dests[3].name) == "Food");   // unnamed on the map
    CHECK(box->dests[3].kind == PoiFood);

    CASE("a new map file replaces the open one; a pulled one is NoMap");
    card.files[services::kMapPath] = map_bytes(20261201);
    serve(*box, *eng, card, RouteMailbox::Op::Open);
    CHECK(box->map_ok && box->map_date == 20261201);
    card.files.erase(services::kMapPath);
    serve(*box, *eng, card, RouteMailbox::Op::Query);
    CHECK(!box->map_ok);
    CHECK(box->status == Status::NoMap);

    CASE("a damaged map is not opened");
    {
        std::vector<uint8_t> bad = map_bytes(20261102);
        bad[0] = 'Z';
        card.files[services::kMapPath] = bad;
        serve(*box, *eng, card, RouteMailbox::Op::Open);
        CHECK(!box->map_ok);
    }

    return check::report("route_store");
}
