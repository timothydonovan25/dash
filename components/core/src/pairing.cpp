#include "core/ble/pairing.hpp"

#include <cstring>

namespace core::ble {

bool operator==(const PeerId& a, const PeerId& b) {
    return a.type == b.type && std::memcmp(a.addr, b.addr, sizeof(a.addr)) == 0;
}

bool Peers::contains(const PeerId& p) const {
    for (size_t i = 0; i < n_; ++i) {
        if (p_[i] == p) return true;
    }
    return false;
}

bool Peers::add(const PeerId& p) {
    if (n_ >= kMaxPeers || contains(p)) return false;
    p_[n_++] = p;
    return true;
}

void Peers::remove(const PeerId& p) {
    for (size_t i = 0; i < n_; ++i) {
        if (p_[i] == p) {
            p_[i] = p_[--n_];
            return;
        }
    }
}

void PairingWindow::open(uint32_t now_ms) {
    open_ = true;
    opened_ms_ = now_ms;
}

bool PairingWindow::is_open(uint32_t now_ms) const {
    // Unsigned difference, like every other elapsed time here, so the
    // 49.7-day wrap of the millisecond clock is a non-event.
    return open_ && now_ms - opened_ms_ < kPairingWindowMs;
}

uint32_t PairingWindow::remaining_s(uint32_t now_ms) const {
    if (!is_open(now_ms)) return 0;
    const uint32_t left = kPairingWindowMs - (now_ms - opened_ms_);
    return (left + 999) / 1000;
}

Verdict judge(bool known, size_t peers, bool window_open) {
    if (known) return window_open ? Verdict::Defer : Verdict::Keep;
    if (peers == 0 || window_open) return Verdict::Keep;
    return Verdict::Reject;
}

bool may_evict(size_t peers, bool window_open) {
    return peers == 0 || window_open;
}

bool encrypt_overdue(uint32_t connected_ms, uint32_t now_ms, bool encrypted) {
    return !encrypted && now_ms - connected_ms >= kEncryptTimeoutMs;
}

}  // namespace core::ble
