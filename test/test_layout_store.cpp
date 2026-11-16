// services::layout_store: layouts on the card, against a card in RAM.
#include "check.hpp"
#include "services/layout_store.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace services;
namespace lay = core::layout;

namespace {
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
    hal::Status remove(const char* path) override { files.erase(path); return hal::Status::Ok; }
    hal::Status write_file(const char* path, const void* data, size_t len) override {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        files[path] = std::vector<uint8_t>(p, p + len);
        return hal::Status::Ok;
    }
    hal::Status rename(const char* from, const char* to) override {
        if (files.count(to)) return hal::Status::Busy;
        auto it = files.find(from);
        if (it == files.end()) return hal::Status::NotPresent;
        files[to] = it->second;
        files.erase(it);
        return hal::Status::Ok;
    }
};

std::vector<uint8_t> enduro_bytes() {
    static lay::Deck d;
    lay::default_deck(d);
    std::strcpy(d.modes[0].name, "ENDURO");
    std::vector<uint8_t> b(lay::kMaxEncoded);
    b.resize(lay::encode(d, b.data(), b.size()));
    return b;
}

LayoutMailbox& box() {
    static LayoutMailbox* b = new LayoutMailbox;
    return *b;
}
}  // namespace

int main() {
    CASE("file names");
    {
        CHECK(valid_ui_name("enduro.dui"));
        CHECK(valid_ui_name("track-day-2.dui"));
        CHECK(!valid_ui_name("Enduro.dui"));
        CHECK(!valid_ui_name("../active.dui"));
        CHECK(!valid_ui_name(".dui"));
        CHECK(!valid_ui_name("enduro.bin"));
        CHECK(!valid_ui_name("a-very-long-name-xx.dui"));
        CHECK(!valid_ui_name(nullptr));
    }

    CASE("no active file is stock; a good one loads; a bad one is refused with why");
    {
        RamCard card;
        static lay::Deck out;
        static uint8_t buf[lay::kMaxEncoded];
        char name[kUiNameLen];
        lay::Error why;
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Stock);

        const std::vector<uint8_t> b = enduro_bytes();
        CHECK(hal::ok(write_ui_file(card, "enduro.dui", b.data(), b.size())));
        CHECK(card.files.count("ui/upload.tmp") == 0);
        CHECK(hal::ok(set_active(card, "enduro.dui")));
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Loaded);
        CHECK(std::string(name) == "enduro.dui");
        CHECK(std::string(out.modes[0].name) == "ENDURO");

        // Saved again over itself: still one file, still good.
        CHECK(hal::ok(write_ui_file(card, "enduro.dui", b.data(), b.size())));
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Loaded);

        // A laptop's editor left Windows line endings.
        card.files["ui/active"] = {'e', 'n', 'd', 'u', 'r', 'o', '.', 'd', 'u', 'i', '\r', '\n'};
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Loaded);

        card.files["ui/enduro.dui"][12] ^= 0xFF;
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Refused);
        CHECK(std::string(why.what) == "damaged (CRC)");

        CHECK(hal::ok(set_active(card, "gone.dui")));
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Refused);
        CHECK(std::string(why.what) == "the active layout is missing");

        card.files["ui/active"] = {'.', '.', '/', 'x'};
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Refused);

        CHECK(hal::ok(set_active(card, "")));
        CHECK(load_active(card, buf, sizeof buf, out, name, sizeof name, &why) == LoadResult::Stock);
    }

    CASE("serve_layout posts on mount, waits for render, reads again on reload");
    {
        RamCard card;
        LayoutMailbox& b = box();
        const std::vector<uint8_t> bytes = enduro_bytes();
        write_ui_file(card, "enduro.dui", bytes.data(), bytes.size());
        set_active(card, "enduro.dui");

        serve_layout(b, card, false);           // nothing asked: nothing posted
        CHECK(b.card.free());
        serve_layout(b, card, true);            // mounted
        CHECK(b.card.waiting());
        CHECK(!b.card.stock);
        CHECK(std::string(b.active) == "enduro.dui");

        // Asked again before render has taken it: owed, not overwritten.
        set_active(card, "");
        b.reload = true;
        serve_layout(b, card, false);
        CHECK(std::string(b.card.deck.modes[0].name) == "ENDURO");
        b.card.take();                          // render copies it out
        serve_layout(b, card, false);           // now the owed read
        CHECK(b.card.waiting());
        CHECK(b.card.stock);
        CHECK(std::string(b.active).empty());

        b.card.take();
        set_active(card, "gone.dui");
        b.reload = true;
        serve_layout(b, card, false);
        CHECK(b.card.stock);
        CHECK(std::strstr(b.error, "missing") != nullptr);
    }

    CASE("render follows: the card at boot keeps the place, a draft wins, then the card again");
    {
        LayoutMailbox& b = *new LayoutMailbox;
        static lay::Deck live;
        LayoutFollower f;
        CHECK(!f.step(b, live, 100, 0x0102).apply);    // nothing posted: stock stays

        lay::default_deck(b.card.deck);
        std::strcpy(b.card.deck.modes[0].name, "CARD");
        b.card.stock = false;
        b.card.post();
        LayoutFollower::Change c = f.step(b, live, 800, 0x0102);
        CHECK(c.apply && c.deck == &live && c.place == 0x0102);
        CHECK(std::string(live.modes[0].name) == "CARD");
        CHECK(b.card.free());

        // The editor's draft, showing its screen.
        lay::default_deck(b.draft.deck);
        std::strcpy(b.draft.deck.modes[0].name, "DRAFT");
        b.draft.place = 0x0300;
        b.draft.post();
        c = f.step(b, live, 10000, 0);
        CHECK(c.apply && c.place == 0x0300);
        CHECK(std::string(live.modes[0].name) == "DRAFT");
        CHECK(f.draft_on());

        // The card answering meanwhile is held, not shown over the draft.
        b.card.post();
        CHECK(!f.step(b, live, 11000, 0).apply);
        CHECK(b.card.waiting());

        // 30 s without an edit: the draft ends and the card is asked again.
        CHECK(!f.step(b, live, 10000 + kDraftIdleMs + 1, 0).apply ||
              std::string(live.modes[0].name) == "CARD");
        CHECK(!f.draft_on());
        CHECK(b.reload.load());
        b.card.take();   // as if render had applied the held one; now a fresh answer
        b.card.post();
        c = f.step(b, live, 41000, 0x0102);
        CHECK(c.apply && c.place == 0);                 // long after boot: home
        CHECK(std::string(live.modes[0].name) == "CARD");

        // Stock answered while stock is not showing: applied once, not twice.
        b.card.stock = true;
        b.card.post();
        c = f.step(b, live, 42000, 0);
        CHECK(c.apply && c.deck == nullptr);
        b.card.post();
        CHECK(!f.step(b, live, 43000, 0).apply);

        // The page ends a draft at once (Save, or Live off).
        b.draft.post();
        CHECK(f.step(b, live, 44000, 0).apply);
        b.draft_end = true;
        f.step(b, live, 44100, 0);
        CHECK(!f.draft_on());
    }

    return check::report("layout_store");
}
