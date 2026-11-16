// Custom layouts on the card (services/layout_store.hpp).
#include "services/layout_store.hpp"
#include <cstdio>
#include <cstring>

namespace services {

namespace lay = core::layout;

bool valid_ui_name(const char* name) {
    if (!name) return false;
    const size_t n = std::strlen(name);
    if (n < 5 || n > 20 || std::strcmp(name + n - 4, ".dui") != 0) return false;
    for (size_t i = 0; i + 4 < n; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

namespace {
// The whole of a small file, or false.
bool read_whole(hal::ICard& card, const char* path, uint8_t* buf, size_t cap, size_t& n) {
    n = 0;
    size_t size = 0;
    if (!hal::ok(card.size_of(path, size)) || size > cap) return false;
    while (n < size) {
        size_t got = 0;
        if (!hal::ok(card.read_at(path, n, buf + n, size - n, got)) || got == 0) return false;
        n += got;
    }
    return true;
}

void copy(char* dst, size_t n, const char* s) { std::snprintf(dst, n, "%s", s); }
}  // namespace

LoadResult load_active(hal::ICard& card, uint8_t* buf, size_t cap, lay::Deck& out,
                       char* name, size_t name_n, lay::Error* why) {
    copy(name, name_n, "");
    size_t n = 0;
    char active[kUiNameLen + 2] = {};
    if (!read_whole(card, kUiActivePath, reinterpret_cast<uint8_t*>(active), sizeof active - 1, n))
        return LoadResult::Stock;
    active[n] = '\0';
    // One line; a laptop editor may have added \r\n or spaces.
    for (char* p = active; *p; ++p) {
        if (*p == '\r' || *p == '\n' || *p == ' ') { *p = '\0'; break; }
    }
    if (!active[0]) return LoadResult::Stock;
    copy(name, name_n, active);
    if (!valid_ui_name(active)) {
        if (why) *why = lay::Error{"ui/active: not a .dui name"};
        return LoadResult::Refused;
    }
    char path[48];
    std::snprintf(path, sizeof path, "%s/%s", kUiDir, active);
    if (!read_whole(card, path, buf, cap, n)) {
        if (why) *why = lay::Error{"the active layout is missing"};
        return LoadResult::Refused;
    }
    return lay::decode(buf, n, out, why) ? LoadResult::Loaded : LoadResult::Refused;
}

hal::Status write_ui_file(hal::ICard& card, const char* name, const uint8_t* data, size_t n) {
    char path[48], tmp[48];
    std::snprintf(path, sizeof path, "%s/%s", kUiDir, name);
    std::snprintf(tmp, sizeof tmp, "%s/upload.tmp", kUiDir);
    const hal::Status s = card.write_file(tmp, data, n);
    if (!hal::ok(s)) return s;
    card.remove(path);
    const hal::Status r = card.rename(tmp, path);
    if (!hal::ok(r)) card.remove(tmp);
    return r;
}

hal::Status set_active(hal::ICard& card, const char* name) {
    if (!name || !name[0]) {
        card.remove(kUiActivePath);
        return hal::Status::Ok;
    }
    char line[kUiNameLen + 1];
    const int n = std::snprintf(line, sizeof line, "%s\n", name);
    char tmp[32];
    std::snprintf(tmp, sizeof tmp, "%s/active.tmp", kUiDir);
    const hal::Status s = card.write_file(tmp, line, static_cast<size_t>(n));
    if (!hal::ok(s)) return s;
    card.remove(kUiActivePath);
    const hal::Status r = card.rename(tmp, kUiActivePath);
    if (!hal::ok(r)) card.remove(tmp);
    return r;
}

LayoutFollower::Change LayoutFollower::step(LayoutMailbox& box, lay::Deck& live,
                                            uint32_t now, uint16_t boot_place) {
    Change c;
    if (box.draft.waiting()) {
        live = box.draft.deck;
        c.place = box.draft.place;
        box.draft.take();
        box.draft_end.store(false, std::memory_order_relaxed);
        draft_on_ = true;
        draft_ms_ = now;
        showing_stock_ = false;
        c.apply = true;
        c.deck = &live;
        return c;
    }
    const bool ended = box.draft_end.exchange(false, std::memory_order_relaxed);
    if (draft_on_ && (ended || now - draft_ms_ > kDraftIdleMs)) {
        // Back to the card's: whatever it says now, read afresh. The draft
        // stays on the glass the half second that takes.
        draft_on_ = false;
        box.reload.store(true, std::memory_order_relaxed);
        showing_stock_ = false;   // so even a stock answer is applied
    }
    if (draft_on_ || !box.card.waiting()) return c;

    const bool stock = box.card.stock;
    if (!stock) live = box.card.deck;
    box.card.take();
    const bool first = !answered_ && now < kBootWindowMs;
    answered_ = true;
    if (stock && showing_stock_) return c;   // already stock: no jump home
    showing_stock_ = stock;
    c.apply = true;
    c.deck = stock ? nullptr : &live;
    c.place = first ? boot_place : 0;
    return c;
}

void serve_layout(LayoutMailbox& box, hal::ICard& card, bool mounted_now) {
    if (box.stock_req.exchange(false)) {
        set_active(card, "");
        box.pending = true;
    }
    if (mounted_now || box.reload.exchange(false)) box.pending = true;
    if (!box.pending || !box.card.free()) return;
    box.pending = false;

    lay::Error why;
    char name[kUiNameLen];
    const LoadResult r = load_active(card, box.buf, sizeof box.buf, box.card.deck, name, sizeof name, &why);
    box.card.stock = r != LoadResult::Loaded;
    copy(box.active, sizeof box.active, r == LoadResult::Loaded ? name : "");
    if (r == LoadResult::Refused) {
        std::snprintf(box.error, sizeof box.error, "%s: %s", name, why.what);
    } else {
        box.error[0] = '\0';
    }
    box.card.post();
}

}  // namespace services
