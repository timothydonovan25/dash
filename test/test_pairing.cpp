#include "check.hpp"
#include "core/ble/pairing.hpp"

using namespace core::ble;

namespace {
PeerId peer(uint8_t last) {
    PeerId p;
    p.type = 1;
    p.addr[5] = last;
    return p;
}
}  // namespace

int main() {
    CASE("the first phone ever bonds without a pairing window");
    {
        CHECK(judge(false, 0, false) == Verdict::Keep);
    }

    CASE("a new phone outside a window is rejected once one is bonded");
    {
        CHECK(judge(false, 1, false) == Verdict::Reject);
        CHECK(judge(false, kMaxPeers, false) == Verdict::Reject);
    }

    CASE("a new phone inside a window is kept");
    {
        CHECK(judge(false, 1, true) == Verdict::Keep);
    }

    CASE("a bonded phone is kept, but deferred while a window is open");
    {
        CHECK(judge(true, 1, false) == Verdict::Keep);
        // Otherwise the rider's own phone reconnects the moment the window
        // opens and the new one never gets the single connection.
        CHECK(judge(true, 1, true) == Verdict::Defer);
    }

    CASE("NimBLE may only evict an old bond when a new phone is welcome");
    {
        CHECK(may_evict(0, false));
        CHECK(may_evict(3, true));
        CHECK(!may_evict(3, false));
    }

    CASE("the pairing window opens, counts down and closes on time");
    {
        PairingWindow w;
        CHECK(!w.is_open(0));
        CHECK(w.remaining_s(0) == 0);

        w.open(1000);
        CHECK(w.is_open(1000));
        CHECK(w.remaining_s(1000) == kPairingWindowMs / 1000);
        // Rounded up: the last part-second still shows as one.
        CHECK(w.remaining_s(1000 + kPairingWindowMs - 1) == 1);
        CHECK(w.is_open(1000 + kPairingWindowMs - 1));
        CHECK(!w.is_open(1000 + kPairingWindowMs));
        CHECK(w.remaining_s(1000 + kPairingWindowMs) == 0);

        w.open(5000);
        w.close();
        CHECK(!w.is_open(5001));
    }

    CASE("the pairing window survives the millisecond clock wrapping");
    {
        PairingWindow w;
        const uint32_t near_wrap = 0xFFFFFFFFu - 1000;
        w.open(near_wrap);
        CHECK(w.is_open(near_wrap + 20000));     // wrapped past zero
        CHECK(w.remaining_s(near_wrap + 20000) == 40);
    }

    CASE("an unencrypted link is dropped only after the timeout");
    {
        CHECK(!encrypt_overdue(100, 100 + kEncryptTimeoutMs - 1, false));
        CHECK(encrypt_overdue(100, 100 + kEncryptTimeoutMs, false));
        CHECK(!encrypt_overdue(100, 100 + kEncryptTimeoutMs * 2, true));
    }

    CASE("the peer cache adds, finds, refuses duplicates and fills up");
    {
        Peers ps;
        CHECK(ps.size() == 0);
        CHECK(ps.add(peer(1)));
        CHECK(!ps.add(peer(1)));
        CHECK(ps.contains(peer(1)));
        CHECK(!ps.contains(peer(2)));

        PeerId other_type = peer(1);
        other_type.type = 0;          // public and random are different peers
        CHECK(!ps.contains(other_type));

        for (uint8_t i = 2; i <= kMaxPeers; ++i) CHECK(ps.add(peer(i)));
        CHECK(ps.size() == kMaxPeers);
        CHECK(!ps.add(peer(99)));

        ps.remove(peer(1));
        CHECK(!ps.contains(peer(1)));
        CHECK(ps.contains(peer(2)));
        CHECK(ps.size() == kMaxPeers - 1);
        ps.remove(peer(42));          // not there: nothing happens
        CHECK(ps.size() == kMaxPeers - 1);

        ps.clear();
        CHECK(ps.size() == 0);
        CHECK(!ps.contains(peer(2)));
    }

    CASE("the bond store writes nothing while the bike is moving");
    {
        CHECK(!may_store(false, false, true));
        CHECK(!may_store(false, true, true));
        CHECK(may_store(true, false, false));      // a bonded phone's CCCD, parked
    }

    CASE("a stranger's records are never written outside a window");
    {
        CHECK(!may_store(true, true, false));
        CHECK(may_store(true, true, true));        // the phone being paired
    }

    CASE("a new phone is welcome exactly when judge would keep it");
    {
        CHECK(new_phone_welcome(0, false));
        CHECK(!new_phone_welcome(1, false));
        CHECK(new_phone_welcome(1, true));
        CHECK(new_phone_welcome(3, true));
    }

    return check::report("pairing");
}
