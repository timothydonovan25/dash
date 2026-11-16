// The screen editor's JSON, on the laptop (services/editor_info.hpp).
//
//   screens_json fields            what /api/fields serves
//   screens_json values 1,14,23    what /api/values serves, for a bike at speed
//   screens_json check < x.dui     decode and validate, as the dash does: {"ok":..}
//   screens_json stock > s.dui     the compiled-in layout, encoded
//
// tools/screens/dev_server.py serves the editor with these, against the sim's
// card, so the page can be built and tried without the board.
#include "services/editor_info.hpp"
#include "core/layout/deck.hpp"
#include "core/state/slots.hpp"
#include <cstdio>
#include <cstring>

namespace {
void emit(const char* s, size_t n, void*) { std::fwrite(s, 1, n, stdout); }
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "fields") == 0) {
        services::write_editor_info(emit, nullptr);
        std::putchar('\n');
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "values") == 0) {
        static core::state::Frame f{};
        for (uint8_t i = 0; i < core::state::kDevCount; ++i)
            f.health.dev[i].state = core::state::DevState::Ok;
        f.engine.rpm = 8234; f.engine.gear = 3; f.engine.tps_pct = 62;
        f.motion.best_mph = 41.6f; f.motion.front_mph = 41.2f; f.motion.rear_mph = 43.9f;
        f.att.lean_deg = 23; f.att.pitch_deg = -2;
        f.elec.vbat = 13.4f; f.gnss.fix = true; f.gnss.sats = 11; f.gnss.utc_ms = 52200000;
        f.trip.trip_a_mi = 47.3f; f.trip.odo_mi = 1832; f.trip.eng_hours = 61.4f;
        f.lap.current_ms = 83456; f.lap.best_ms = 81230; f.lap.last_ms = 82110;
        f.tpms.front.psi = 14.5f; f.tpms.rear.psi = 12.0f; f.heart.bpm = 142;
        std::strcpy(f.media.title, "Flying Whales");
        uint16_t ids[128];
        const size_t n = services::parse_ids(argv[2], ids, 128);
        core::layout::ShowCtx c;
        services::write_values(f, c, ids, n, emit, nullptr);
        std::putchar('\n');
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "check") == 0) {
        static uint8_t buf[core::layout::kMaxEncoded + 1];
        const size_t n = std::fread(buf, 1, sizeof buf, stdin);
        static core::layout::Deck d;
        core::layout::Error why;
        if (n > core::layout::kMaxEncoded) why.what = "not a layout file";
        if (n <= core::layout::kMaxEncoded && core::layout::decode(buf, n, d, &why)) {
            std::printf("{\"ok\":true}\n");
            return 0;
        }
        std::printf("{\"ok\":false,\"error\":\"%s\",\"index\":%d}\n", why.what, why.index);
        return 1;
    }
    if (argc >= 2 && std::strcmp(argv[1], "stock") == 0) {
        static core::layout::Deck d;
        core::layout::default_deck(d);
        static uint8_t buf[core::layout::kMaxEncoded];
        std::fwrite(buf, 1, core::layout::encode(d, buf, sizeof buf), stdout);
        return 0;
    }
    std::fprintf(stderr, "usage: screens_json fields | values <ids> | check | stock\n");
    return 2;
}
