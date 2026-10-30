// Heart rate from a watch: the measurement parser, the advert parser, which
// sensor to connect to, and the session numbers.
#include "check.hpp"
#include "core/hr/hr.hpp"

#include <cstring>

using namespace core::hr;

namespace {
// A pass every 20 ms from `from` for `ms`, with `set` shaping each input.
// The heart rate falls 0.5 bpm a second from 160 once the bike stops.
template <class F>
bool drive(Recovery& r, uint32_t from, uint32_t ms, F set) {
    bool any = false;
    for (uint32_t t = from; t < from + ms; t += 20) {
        RecoveryInput in;
        in.t_ms = t;
        in.speed_ok = true;
        in.engine_running = true;
        set(in);
        any = r.update(in) || any;
    }
    return any;
}
void riding(RecoveryInput& in) { in.mph = 25.0f; in.bpm = 160; }
Addr addr(uint8_t last, uint8_t type = 0) {
    Addr a;
    a.type = type;
    a.val[0] = last;
    a.val[5] = 0xC0;
    return a;
}
}  // namespace

int main() {
    CASE("an 8-bit reading with contact");
    {
        const uint8_t p[] = {0x06, 72};
        Measurement m;
        CHECK(parse(p, sizeof p, m));
        CHECK(m.bpm == 72);
        CHECK(m.contact_known);
        CHECK(m.contact);
        CHECK(m.rr_count == 0);
    }

    CASE("contact supported but lost");
    {
        const uint8_t p[] = {0x04, 90};
        Measurement m;
        CHECK(parse(p, sizeof p, m));
        CHECK(m.contact_known);
        CHECK(!m.contact);
    }

    CASE("a 16-bit reading, energy and RR intervals skipped and read");
    {
        // flags: uint16 bpm, energy, RR. bpm 0x00B4 = 180, energy 0x1234,
        // RR 1024/1024 s = 1000 ms and 512 = 500 ms.
        const uint8_t p[] = {0x19, 0xB4, 0x00, 0x34, 0x12, 0x00, 0x04, 0x00, 0x02};
        Measurement m;
        CHECK(parse(p, sizeof p, m));
        CHECK(m.bpm == 180);
        CHECK(!m.contact_known);
        CHECK(m.rr_count == 2);
        CHECK(m.rr_ms[0] == 1000);
        CHECK(m.rr_ms[1] == 500);
    }

    CASE("short packets are refused; a zero is a reading with no pulse");
    {
        Measurement m;
        const uint8_t one[] = {0x00};
        CHECK(!parse(one, sizeof one, m));
        const uint8_t wide[] = {0x01, 0x50};          // says uint16, has one byte
        CHECK(!parse(wide, sizeof wide, m));
        CHECK(!parse(nullptr, 4, m));
        const uint8_t zero[] = {0x00, 0x00};
        CHECK(parse(zero, sizeof zero, m));
        CHECK(m.bpm == 0 && !m.pulse());
    }

    CASE("a Garmin off the wrist: its last bpm with the contact bit clear");
    {
        // Captured from a D2 Air X10, 2026-10-28: worn, then taken off.
        Measurement m;
        const uint8_t worn[] = {0x06, 0x61};
        CHECK(parse(worn, sizeof worn, m));
        CHECK(m.bpm == 97 && m.pulse());
        const uint8_t off[] = {0x04, 0x5E};
        CHECK(parse(off, sizeof off, m));
        CHECK(m.bpm == 94 && m.contact_known && !m.contact);
        CHECK(!m.pulse());
        const uint8_t no_contact_bits[] = {0x00, 0x48};   // a strap that does not say
        CHECK(parse(no_contact_bits, sizeof no_contact_bits, m));
        CHECK(m.pulse());
    }

    CASE("an advert with the Heart Rate Service and a name");
    {
        const uint8_t ad[] = {
            0x02, 0x01, 0x06,                     // flags
            0x05, 0x03, 0x0D, 0x18, 0x0F, 0x18,   // 0x180D, 0x180F
            0x0B, 0x09, 'D', '2', ' ', 'A', 'i', 'r', ' ', 'X', '1', '0',
        };
        const Advert a = parse_advert(ad, sizeof ad);
        CHECK(a.hrs);
        CHECK(std::strcmp(a.name, "D2 Air X10") == 0);
    }

    CASE("an advert without it, and a truncated one");
    {
        const uint8_t ad[] = {0x03, 0x03, 0x0F, 0x18, 0x09, 0x09, 'x'};   // last runs off
        const Advert a = parse_advert(ad, sizeof ad);
        CHECK(!a.hrs);
        CHECK(a.name[0] == '\0');
        const uint8_t long_name[] = {0x1A, 0x09, 'A','B','C','D','E','F','G','H','I','J','K','L',
                                     'M','N','O','P','Q','R','S','T','U','V','W','X','Y'};
        const Advert b = parse_advert(long_name, sizeof long_name);
        CHECK(std::strlen(b.name) == sizeof b.name - 1);
    }

    CASE("the strongest sensor above the floor is chosen");
    {
        Picker p;
        p.begin_scan();
        p.heard(addr(1), -78, "Strap");
        p.heard(addr(2), -55, "D2 Air X10");
        p.heard(addr(3), -95, "Close but no");     // under the floor
        Addr out;
        CHECK(p.choice(out));
        CHECK(same(out, addr(2)));
    }

    CASE("nothing above the floor is nothing");
    {
        Picker p;
        p.begin_scan();
        p.heard(addr(3), -95, "Far");
        Addr out;
        CHECK(!p.choice(out));
    }

    CASE("once latched, only that sensor, however weak, by address or name");
    {
        Picker p;
        p.begin_scan();
        p.heard(addr(2), -55, "D2 Air X10");
        p.latch(addr(2), nullptr);
        CHECK(p.latched());
        CHECK(std::strcmp(p.name(), "D2 Air X10") == 0);

        p.begin_scan();
        p.heard(addr(1), -40, "Strap");            // stronger, someone else's
        p.heard(addr(2), -92, "D2 Air X10");       // below the floor, still ours
        Addr out;
        CHECK(p.choice(out));
        CHECK(same(out, addr(2)));

        // The watch came back on a new private address.
        p.begin_scan();
        p.heard(addr(1), -40, "Strap");
        p.heard(addr(9, 1), -60, "D2 Air X10");
        CHECK(p.choice(out));
        CHECK(same(out, addr(9, 1)));

        p.reset();
        CHECK(!p.latched());
        p.begin_scan();
        p.heard(addr(1), -40, "Strap");
        CHECK(p.choice(out));
        CHECK(same(out, addr(1)));
    }

    CASE("session average, max and min");
    {
        Session s;
        CHECK(s.avg_bpm() == 0);
        s.add(100);
        s.add(150);
        s.add(0);                 // ignored
        s.add(121);
        CHECK(s.count() == 3);
        CHECK(s.avg_bpm() == 124);
        CHECK(s.max_bpm() == 150);
        CHECK(s.min_bpm() == 100);
    }

    CASE("a reading is fresh for five seconds while live");
    {
        core::state::Heart h;
        h.link = core::state::Heart::Live;
        h.bpm = 120;
        h.at_ms = 10000;
        CHECK(fresh(h, 10000));
        CHECK(fresh(h, 14999));
        CHECK(!fresh(h, 15000));
        // The frame's clock was read just before the reading arrived.
        CHECK(fresh(h, 9996));
        h.link = core::state::Heart::Searching;
        CHECK(!fresh(h, 10001));
    }

    CASE("no pulse: live, worn on this link, readings arriving, none with a pulse");
    {
        core::state::Heart h;
        h.link = core::state::Heart::Live;
        h.bpm = 0;
        h.at_ms = 10000;
        CHECK(!no_pulse(h, 10500));          // never worn on this link: just put down
        h.worn = true;
        CHECK(no_pulse(h, 10500));
        CHECK(no_pulse(h, 9997));            // stamped just ahead of the frame
        CHECK(!no_pulse(h, 10000 + kStaleMs));   // gone quiet: says nothing
        h.bpm = 88;
        CHECK(!no_pulse(h, 10500));          // a pulse
        h.bpm = 0;
        h.link = core::state::Heart::Searching;
        CHECK(!no_pulse(h, 10500));
    }

    CASE("ten minutes of history: a sample a step, gaps where there was no pulse");
    {
        core::state::Heart h;
        h.link = core::state::Heart::Live;
        History hist;
        uint8_t lo = 0, hi = 0;
        CHECK(!hist.range(lo, hi));

        // 100 bpm for a minute, a frame every 20 ms.
        uint32_t t = 1000;
        for (; t < 61000; t += 20) {
            h.bpm = 100;
            h.at_ms = t;
            hist.update(h, t);
        }
        CHECK(hist.at(History::kSamples - 1) == 100);
        CHECK(hist.at(History::kSamples - 31) == 100);   // 1 s to 61 s: 31 steps
        CHECK(hist.at(History::kSamples - 32) == 0);     // before it started
        const uint32_t seq = hist.seq();
        hist.update(h, t);                                // the same step: nothing
        CHECK(hist.seq() == seq);

        // Off the wrist for 10 s, then 130.
        for (; t < 71000; t += 20) {
            h.bpm = 0;
            h.at_ms = t;
            hist.update(h, t);
        }
        t = 72000;                                        // the next step
        h.bpm = 130;
        h.at_ms = t;
        hist.update(h, t);
        CHECK(hist.at(History::kSamples - 1) == 130);
        CHECK(hist.at(History::kSamples - 2) == 0);
        CHECK(hist.range(lo, hi) && lo == 100 && hi == 130);

        // No frames for 20 s (render stalled): ten gaps, then the reading.
        t += 20000;
        h.at_ms = t;
        hist.update(h, t);
        CHECK(hist.at(History::kSamples - 1) == 130);
        CHECK(hist.at(History::kSamples - 2) == 0 && hist.at(History::kSamples - 10) == 0);
        CHECK(hist.at(History::kSamples - 11) == 130);

        // An hour away: all gaps but the last.
        t += 3600000;
        h.at_ms = t;
        hist.update(h, t);
        CHECK(hist.range(lo, hi) && lo == 130 && hi == 130);
        CHECK(hist.at(0) == 0);
    }

    CASE("recovery: a ride, then parked in neutral: the fall at one minute and two");
    {
        Recovery r;
        drive(r, 0, 180000, riding);                       // three minutes
        const uint32_t stop = 180000;
        auto parked = [&](RecoveryInput& in) {
            in.mph = 0.0f;
            in.neutral = in.t_ms >= stop + 4000;           // a few seconds to find neutral
            in.bpm = static_cast<uint16_t>(160 - (in.t_ms - stop) / 2000);
        };
        CHECK(!drive(r, stop, 30000, parked));
        CHECK(r.publish().measuring);
        CHECK(r.publish().start_bpm == 160);
        CHECK(r.publish().live_drop >= 14 && r.publish().live_drop <= 15);
        CHECK(drive(r, stop + 30000, 31000, parked));      // the minute
        CHECK(r.publish().drop1 == 30);
        CHECK(r.publish().count == 1 && r.publish().best_drop1 == 30);
        CHECK(!r.publish().has_drop2);
        CHECK(drive(r, stop + 61000, 60000, parked));      // and the second
        CHECK(r.publish().has_drop2 && r.publish().drop2 == 60);
        CHECK(!r.publish().measuring);
        CHECK(r.publish().seq == 2);
    }

    CASE("recovery: an engine switched off counts as parked, in gear or not");
    {
        Recovery r;
        drive(r, 0, 150000, riding);
        CHECK(drive(r, 150000, 61000, [](RecoveryInput& in) {
            in.mph = 0.0f; in.engine_running = false; in.bpm = 140;
        }));
        CHECK(r.publish().drop1 == 0 && r.publish().count == 1);
    }

    CASE("recovery: tight technical riding never counts");
    {
        Recovery r;
        drive(r, 0, 150000, riding);
        // Stop and go at walking pace, feet down, in gear with it running:
        // stops of up to 40 s, never parked.
        uint32_t t = 150000;
        for (int i = 0; i < 10; ++i) {
            CHECK(!drive(r, t, 8000, [](RecoveryInput& in) { in.mph = 3.0f; in.bpm = 150; }));
            t += 8000;
            CHECK(!drive(r, t, 40000, [](RecoveryInput& in) { in.mph = 0.0f; in.bpm = 150; }));
            t += 40000;
        }
        CHECK(r.publish().count == 0 && !r.publish().measuring);
        // A stall in a section: engine off for a moment, then off again in gear.
        CHECK(!drive(r, t, 3000, [](RecoveryInput& in) { in.mph = 0.0f; in.engine_running = false; in.bpm = 150; }));
        t += 3000;
        CHECK(!drive(r, t, 60000, [](RecoveryInput& in) { in.mph = 2.0f; in.bpm = 150; }));
        CHECK(r.publish().count == 0);
    }

    CASE("recovery: moving off, no ride first, or no pulse at the minute: no result");
    {
        auto park = [](RecoveryInput& in) { in.mph = 0.0f; in.neutral = true; in.bpm = 150; };
        Recovery a;
        drive(a, 0, 150000, riding);
        CHECK(!drive(a, 150000, 30000, park));
        CHECK(!drive(a, 180000, 5000, riding));            // rode off at 30 s
        CHECK(a.publish().count == 0 && !a.publish().measuring);

        Recovery b;
        drive(b, 0, 60000, riding);                        // a minute is not a ride
        CHECK(!drive(b, 60000, 70000, park));
        CHECK(b.publish().count == 0);

        Recovery c;
        drive(c, 0, 150000, riding);
        CHECK(!drive(c, 150000, 59000, park));
        CHECK(!drive(c, 209000, 3000, [](RecoveryInput& in) { in.mph = 0.0f; in.neutral = true; }));
        CHECK(c.publish().count == 0);

        // After a result, the next needs another ride.
        Recovery d;
        drive(d, 0, 150000, riding);
        CHECK(drive(d, 150000, 61000, park));
        drive(d, 211000, 30000, riding);
        CHECK(!drive(d, 241000, 70000, park));
        CHECK(d.publish().count == 1);

        // A short stop in neutral that rides off is not a parked minute: it
        // leaves the ride for the next stop.
        Recovery e;
        drive(e, 0, 150000, riding);
        CHECK(!drive(e, 150000, 10000, park));
        CHECK(!drive(e, 160000, 30000, riding));
        CHECK(drive(e, 190000, 61000, park));
        CHECK(e.publish().count == 1);
    }

    CASE("zones by percent of max");
    {
        CHECK(zone(90, 190) == 0);
        CHECK(zone(95, 190) == 1);
        CHECK(zone(133, 190) == 3);
        CHECK(zone(171, 190) == 5);
        CHECK(zone(150, 0) == 0);
    }

    return check::report("hr");
}
