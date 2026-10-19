// replay — run core/fusion over a ride on a laptop.
//
//   replay ride.csv [options]            a blackbox log off the card
//   replay --scenario <name|all> [opts]  a synthetic ride with a known truth
//
// options:
//   --out FILE        write t, truth, raw, fused, sigma and source bits as CSV
//   --q-speed X  --q-bias X  --r-wheel X  --r-gps X  --gate X  --split X
//   --seed N          for --scenario
//
// The filter is the firmware's own core::fusion::SpeedFilter, so a setting
// that works here works on the bike. A log needs the FrontMPH..GpsUtc
// columns (firmware 1.5 and later); log at 50 Hz (CONFIG -> Log rate) for
// a replay worth tuning on. tools/replay/plot.py draws the --out file.
#include "scenarios.hpp"
#include "core/fusion/speed.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using core::fusion::SpeedConfig;
using core::fusion::SpeedFilter;
using core::fusion::SpeedInput;
using core::fusion::SpeedOutput;

struct Writer {
    std::FILE* f = nullptr;
    void header() {
        if (f) std::fprintf(f, "t,truth,raw,fused,sigma,used,rejected,suspect,front,rear,gps\n");
    }
    void row(const SpeedInput& in, float truth, float raw, const SpeedOutput& o) {
        if (!f) return;
        std::fprintf(f, "%.3f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%.2f,%.2f,%s\n",
                     in.t_ms / 1000.0, truth, raw, o.mph, o.sigma_mph, o.used,
                     o.rejected, o.suspect, in.front_mph, in.rear_mph,
                     in.gps_new ? std::to_string(in.gps_mph).c_str() : "");
    }
};

void print_stats(const char* name, const replay::Error& raw, const replay::Error& shown,
                 const replay::Error& fused, uint16_t resets) {
    std::printf("%-12s raw rms %5.2f max %5.2f   shown rms %5.2f max %5.2f   "
                "fused rms %5.2f max %5.2f   resets %u\n",
                name, raw.rms(), raw.max, shown.rms(), shown.max, fused.rms(), fused.max,
                resets);
}

int run_scenario(replay::Kind k, uint32_t seed, const SpeedConfig& cfg, Writer& w) {
    SpeedFilter filt;
    replay::RawBest best;
    replay::ShownBest shown;
    replay::Error raw_e, shown_e, fused_e;
    SpeedOutput o;
    replay::run(k, seed, [&](const replay::Sample& s) {
        o = filt.step(s.in, cfg);
        const float raw = best(s.in);
        raw_e.add(raw - s.truth_mph);
        shown_e.add(shown(s.in, o) - s.truth_mph);
        fused_e.add((o.valid ? o.mph : 0.0f) - s.truth_mph);
        w.row(s.in, s.truth_mph, raw, o);
    });
    print_stats(replay::kind_name(k), raw_e, shown_e, fused_e, o.resets);
    return 0;
}

// A RaceRender CSV off the card: '#' lines are markers, the first plain
// line names the columns.
int run_log(const char* path, const SpeedConfig& cfg, Writer& w) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "replay: cannot open %s\n", path);
        return 1;
    }
    std::map<std::string, int> col;
    char line[1024];
    SpeedFilter filt;
    replay::RawBest best;
    SpeedOutput o;
    float last_utc = -1.0f;
    size_t rows = 0;
    bool warned = false;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        std::vector<std::string> cells;
        std::string cur;
        for (const char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
            if (*p == ',') { cells.push_back(cur); cur.clear(); }
            else cur += *p;
        }
        cells.push_back(cur);
        if (col.empty()) {
            for (size_t i = 0; i < cells.size(); ++i) col[cells[i]] = static_cast<int>(i);
            if (!col.count("Time")) {
                std::fprintf(stderr, "replay: %s has no Time column\n", path);
                std::fclose(f);
                return 1;
            }
            continue;
        }
        const auto get = [&](const char* name, float dflt) {
            auto it = col.find(name);
            if (it == col.end() || it->second >= static_cast<int>(cells.size()) ||
                cells[it->second].empty()) {
                return dflt;
            }
            return std::strtof(cells[it->second].c_str(), nullptr);
        };
        const bool full = col.count("FrontMPH") && col.count("GpsUtc");
        if (!full && !warned) {
            std::fprintf(stderr, "replay: no raw speed columns (log from before 1.5); "
                                 "using MPH for every source, which proves little\n");
            warned = true;
        }
        SpeedInput in;
        in.t_ms = static_cast<uint32_t>(get("Time", 0.0f) * 1000.0f + 0.5f);
        in.accel_ok = col.count("X") != 0;
        in.accel_g = get("X", 0.0f);
        const float mph = get("MPH", 0.0f);
        in.front_ok = in.rear_ok = true;
        in.front_mph = full ? get("FrontMPH", 0.0f) : mph;
        in.rear_mph = full ? get("RearMPH", 0.0f) : mph;
        const int flags = static_cast<int>(get("Flags", 0.0f));
        in.gps_ok = (flags & 4) != 0;                       // kFlagFix
        in.gps_mph = full ? get("GpsMPH", 0.0f) : mph;
        in.hdop = get("HDOP", 1.0f);
        const float utc = get("GpsUtc", static_cast<float>(rows));
        in.gps_new = in.gps_ok && utc != last_utc;
        last_utc = utc;
        o = filt.step(in, cfg);
        w.row(in, get("FusedMPH", -1.0f), best(in), o);
        ++rows;
    }
    std::fclose(f);
    std::printf("%s: %zu rows, %u resets, accel bias %.3f g\n", path, rows, o.resets, o.bias_g);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SpeedConfig cfg;
    const char* log = nullptr;
    const char* scenario = nullptr;
    const char* out = nullptr;
    uint32_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "replay: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--scenario") scenario = val();
        else if (a == "--out") out = val();
        else if (a == "--seed") seed = static_cast<uint32_t>(std::strtoul(val(), nullptr, 0));
        else if (a == "--q-speed") cfg.q_speed = std::strtof(val(), nullptr);
        else if (a == "--q-bias") cfg.q_bias = std::strtof(val(), nullptr);
        else if (a == "--r-wheel") cfg.r_wheel_mph = std::strtof(val(), nullptr);
        else if (a == "--r-gps") cfg.r_gps_mph = std::strtof(val(), nullptr);
        else if (a == "--gate") cfg.gate_sigma = std::strtof(val(), nullptr);
        else if (a == "--split") cfg.wheel_split_mph = std::strtof(val(), nullptr);
        else if (a[0] != '-') log = argv[i];
        else {
            std::fprintf(stderr, "replay: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    Writer w;
    if (out) {
        w.f = std::fopen(out, "w");
        if (!w.f) {
            std::fprintf(stderr, "replay: cannot write %s\n", out);
            return 1;
        }
        w.header();
    }
    int rc = 0;
    if (scenario) {
        if (std::strcmp(scenario, "all") == 0) {
            for (int k = 0; k < static_cast<int>(replay::Kind::Count); ++k) {
                run_scenario(static_cast<replay::Kind>(k), seed, cfg, w);
            }
        } else {
            replay::Kind k;
            if (!replay::kind_from(scenario, k)) {
                std::fprintf(stderr, "replay: no scenario %s\n", scenario);
                rc = 2;
            } else {
                rc = run_scenario(k, seed, cfg, w);
            }
        }
    } else if (log) {
        rc = run_log(log, cfg, w);
    } else {
        std::fprintf(stderr, "usage: replay ride.csv | --scenario <name|all> [--out f.csv]\n");
        rc = 2;
    }
    if (w.f) std::fclose(w.f);
    return rc;
}
