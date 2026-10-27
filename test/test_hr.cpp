// Heart rate from a watch: the measurement parser, the advert parser, which
// sensor to connect to, and the session numbers.
#include "check.hpp"
#include "core/hr/hr.hpp"

#include <cstring>

using namespace core::hr;

namespace {
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

    CASE("short packets and a zero are refused");
    {
        Measurement m;
        const uint8_t one[] = {0x00};
        CHECK(!parse(one, sizeof one, m));
        const uint8_t wide[] = {0x01, 0x50};          // says uint16, has one byte
        CHECK(!parse(wide, sizeof wide, m));
        const uint8_t zero[] = {0x00, 0x00};
        CHECK(!parse(zero, sizeof zero, m));
        CHECK(!parse(nullptr, 4, m));
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
        h.link = core::state::Heart::Searching;
        CHECK(!fresh(h, 10001));
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
