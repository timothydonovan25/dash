#include "services/track_store.hpp"

namespace services {
namespace {

namespace tr = core::track;

// Reads the whole of `path` into the box's scratch, and how big it was.
// NotPresent for no file or an empty one; Unsupported for one too big to be
// ours; anything else is the card failing, which is worth another try.
hal::Status read_file(hal::ICard& card, const char* path, uint8_t* buf, size_t cap,
                      size_t& len) {
    len = 0;
    size_t size = 0;
    const hal::Status s = card.size_of(path, size);
    if (!hal::ok(s)) return s;
    if (size == 0) return hal::Status::NotPresent;
    if (size > cap) return hal::Status::Unsupported;
    size_t got = 0;
    const hal::Status r = card.read_at(path, 0, buf, size, got);
    if (!hal::ok(r)) return r;
    if (got != size) return hal::Status::IoError;
    len = size;
    return hal::Status::Ok;
}

// A slot's file, or failing that its .tmp: a cut between the two steps of a
// save leaves the new file under the temporary name and nothing under the
// real one. `found` is whether either held a track; the return is false
// only when the card could not be read, so the caller can tell "there is no
// track here" from "ask again".
bool load_slot(TrackMailbox& box, hal::ICard& card, size_t slot, bool points, bool& found) {
    found = false;
    bool readable = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
        char path[32];
        if (!tr::file_name(path, sizeof path, slot, attempt == 1)) return true;
        size_t len = 0;
        const hal::Status st = read_file(card, path, box.file, sizeof box.file, len);
        if (st == hal::Status::NotPresent || st == hal::Status::Unsupported) continue;
        if (!hal::ok(st)) {
            readable = false;
            continue;
        }
        tr::Record r;
        size_t n = 0;
        const bool ok = points ? tr::decode(box.file, len, r, box.pts, core::delta::kMaxPoints, n)
                               : tr::decode_header(box.file, len, r, n);
        // A file that is not a good track is no track: the next save to
        // this slot replaces it.
        if (!ok || !r.used) continue;
        box.rec = r;
        box.n = points ? static_cast<uint16_t>(n) : 0;
        found = true;
        return true;
    }
    return readable;
}

bool save_slot(TrackMailbox& box, hal::ICard& card) {
    char path[32], temp[32];
    if (!tr::file_name(path, sizeof path, box.slot, false) ||
        !tr::file_name(temp, sizeof temp, box.slot, true)) {
        return false;
    }
    const size_t len = tr::encode(box.rec, box.pts, box.n, box.file, sizeof box.file);
    if (len == 0) return false;
    // Written whole under the temporary name, then moved over the old one:
    // a cut at any point leaves one good file, under one name or the other.
    if (!hal::ok(card.write_file(temp, box.file, len))) return false;
    card.remove(path);
    return hal::ok(card.rename(temp, path));
}

}  // namespace

bool serve_tracks(TrackMailbox& box, hal::ICard& card) {
    if (box.turn.load(std::memory_order_acquire) != TrackMailbox::Turn::Posted) return false;
    bool ok = false;
    switch (box.op) {
        case TrackMailbox::Op::List:
            // All or nothing: a library with a hole where the card was busy
            // would hand that slot to the next new track.
            box.lib = tr::Library{};
            ok = true;
            for (size_t i = 0; i < tr::kMaxTracks && ok; ++i) {
                bool found = false;
                ok = load_slot(box, card, i, false, found);
                if (ok && found) box.lib.rec[i] = box.rec;
            }
            if (!ok) box.lib = tr::Library{};
            break;
        case TrackMailbox::Op::Save:
            ok = save_slot(box, card);
            break;
        case TrackMailbox::Op::Load: {
            // ok with no points: there is no reference to have.
            box.n = 0;
            bool found = false;
            ok = load_slot(box, card, box.slot, true, found);
            if (!found) box.rec = tr::Record{};
            break;
        }
        case TrackMailbox::Op::Forget: {
            char path[32];
            for (int t = 0; t < 2; ++t) {
                if (tr::file_name(path, sizeof path, box.slot, t == 1)) card.remove(path);
            }
            ok = true;
            break;
        }
        case TrackMailbox::Op::None:
            break;
    }
    box.ok = ok;
    box.turn.store(TrackMailbox::Turn::Answered, std::memory_order_release);
    return true;
}

}  // namespace services
