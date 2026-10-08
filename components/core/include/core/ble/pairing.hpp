// Which phone the dash lets in (§11.3).
//
// The dash bonds, and Feed and Ctrl need an encrypted link, so the bond is
// the phone's identity: Android rotates its address every fifteen minutes,
// and only the identity key that bonding exchanges can recognise the same
// phone again. Everything here is the policy over that, with no NimBLE in
// it, so the decisions are a unit test rather than a ride. The `ble` service
// does the radio calls; it asks this what to do.
//
// The rules:
//   - The first phone ever bonds freely. A dash fresh off the bench, or one
//     whose bonds were just forgotten, has to be reachable without a menu.
//   - After that a new phone bonds only inside a pairing window, opened from
//     CONFIG. A stranger in the car park cannot take the one connection.
//   - While the window is open, already-bonded phones are turned away, but
//     keep their bond. Otherwise the rider's own phone reconnects in the
//     second the window opens and the new one never gets the slot.
//   - A window admits one phone and closes.
//   - A link that has not encrypted within kEncryptTimeoutMs is dropped, so
//     a scanner that connects and sits there cannot hold the slot either.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::ble {

// How long CONFIG's "Pairing mode" stays open.
inline constexpr uint32_t kPairingWindowMs = 60000;

// Long enough for a rider to find and tap Android's "Pair?" prompt the
// first time; a bonded phone re-encrypts in well under a second.
inline constexpr uint32_t kEncryptTimeoutMs = 30000;

// Matches CONFIG_BT_NIMBLE_MAX_BONDS, which the `ble` service asserts.
inline constexpr size_t kMaxPeers = 3;

// A bonded peer's identity address: NimBLE's ble_addr_t, without NimBLE.
struct PeerId {
    uint8_t type = 0;
    uint8_t addr[6] = {};
};
bool operator==(const PeerId& a, const PeerId& b);

// The bonded phones, as the `ble` task last knew them. A cache of NimBLE's
// own store, kept by the one task that changes it, so the policy below never
// has to ask the store mid-pairing whether a bond is new or old.
class Peers {
public:
    bool   contains(const PeerId& p) const;
    // False if full, or already present.
    bool   add(const PeerId& p);
    void   remove(const PeerId& p);
    void   clear() { n_ = 0; }
    size_t size() const { return n_; }

private:
    PeerId p_[kMaxPeers];
    size_t n_ = 0;
};

class PairingWindow {
public:
    void open(uint32_t now_ms);
    void close() { open_ = false; }
    bool is_open(uint32_t now_ms) const;
    // Whole seconds left, rounded up so the last second shows "1", not "0";
    // 0 when closed.
    uint32_t remaining_s(uint32_t now_ms) const;

private:
    bool     open_ = false;
    uint32_t opened_ms_ = 0;
};

enum class Verdict : uint8_t {
    Keep,     // the phone stays
    Reject,   // a new bond outside a window: delete it and drop the link
    Defer,    // a known phone during a window: drop the link, keep the bond
};

// Called once a link has encrypted. `known` is whether the peer's identity
// was bonded before this connection; `peers` is the count before it.
Verdict judge(bool known, size_t peers, bool window_open);

// Whether NimBLE may evict the oldest bond to make room for a new one. Only
// when a new phone is actually welcome; otherwise a stranger pairing three
// times would push the rider's own phone out of the store.
bool may_evict(size_t peers, bool window_open);

// True when a connected link has gone kEncryptTimeoutMs without encrypting.
bool encrypt_overdue(uint32_t connected_ms, uint32_t now_ms, bool encrypted);

}  // namespace core::ble
