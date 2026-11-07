#include "core/route/graph.hpp"
#include <cstring>

namespace core::route {

BlockCache::BlockCache(uint32_t* tags, uint32_t* stamps, uint8_t* data, uint32_t blocks)
    : tags_(tags), stamps_(stamps), data_(data), sets_(blocks / kWays) {
    attach(nullptr);
}

void BlockCache::attach(IBlockSource* src) {
    src_ = src;
    for (uint32_t i = 0; i < sets_ * kWays; ++i) {
        tags_[i] = kNone;
        stamps_[i] = 0;
    }
    clock_ = misses_ = hits_ = 0;
}

const uint8_t* BlockCache::block(uint32_t index) {
    const uint32_t set = index % sets_;
    uint32_t* tag = tags_ + set * kWays;
    uint32_t* stamp = stamps_ + set * kWays;
    ++clock_;
    uint32_t victim = 0;
    for (uint32_t w = 0; w < kWays; ++w) {
        if (tag[w] == index) {
            stamp[w] = clock_;
            ++hits_;
            return data_ + (set * kWays + w) * size_t(kBlockBytes);
        }
        if (stamp[w] < stamp[victim]) victim = w;
    }
    ++misses_;
    uint8_t* dst = data_ + (set * kWays + victim) * size_t(kBlockBytes);
    const uint32_t off = index * kBlockBytes;
    const uint32_t end = src_->size();
    const uint32_t len = end - off < kBlockBytes ? end - off : kBlockBytes;
    tag[victim] = kNone;
    if (!src_->read(off, dst, len)) { ++errors_; return nullptr; }
    tag[victim] = index;
    stamp[victim] = clock_;
    return dst;
}

bool BlockCache::read(uint32_t offset, void* dst, uint32_t len) {
    if (!src_ || sets_ == 0) return false;
    if (offset > src_->size() || len > src_->size() - offset) return false;
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (len) {
        const uint32_t b = offset / kBlockBytes;
        const uint32_t in = offset % kBlockBytes;
        const uint32_t n = kBlockBytes - in < len ? kBlockBytes - in : len;
        const uint8_t* p = block(b);
        if (!p) return false;
        std::memcpy(out, p + in, n);
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

const char* to_string(OpenResult r) {
    switch (r) {
        case OpenResult::Ok:         return "ok";
        case OpenResult::ReadError:  return "read error";
        case OpenResult::BadMagic:   return "not a map file";
        case OpenResult::BadVersion: return "map file version";
        case OpenResult::BadLayout:  return "map file damaged";
    }
    return "?";
}

bool map_header_ok(const void* first, size_t n, uint32_t* osm_date) {
    if (n < sizeof(Header)) return false;
    Header h;
    std::memcpy(&h, first, sizeof h);
    if (std::memcmp(h.magic, kMagic, 4) != 0 || h.version != kVersion) return false;
    if (h.variant_count == 0 || h.variant_count > kMaxVariants) return false;
    if (osm_date) *osm_date = h.osm_date;
    return true;
}

OpenResult Graph::open(IBlockSource& src) {
    open_ = false;
    cache_.attach(&src);
    if (!read(0, &h_, sizeof h_)) return OpenResult::ReadError;
    if (std::memcmp(h_.magic, kMagic, 4) != 0) return OpenResult::BadMagic;
    if (h_.version != kVersion) return OpenResult::BadVersion;
    const uint64_t size = src.size();
    auto fits = [&](uint32_t off, uint64_t count, uint64_t each) {
        return off + count * each <= size;
    };
    if (h_.variant_count == 0 || h_.variant_count > kMaxVariants ||
        !fits(h_.nodes_off, h_.node_count, sizeof(Point)) ||
        !fits(h_.attrs_off, h_.attr_count, sizeof(Attr)) ||
        !fits(h_.geom_off, h_.geom_count, sizeof(Point)) ||
        !fits(h_.names_off, h_.names_bytes, 1) ||
        !fits(h_.pois_off, h_.poi_count, sizeof(Poi)) ||
        !fits(h_.grid_off, uint64_t(h_.grid_w) * h_.grid_h + 1, 4) ||
        !fits(h_.cells_off, h_.cell_items, 4))
        return OpenResult::BadLayout;
    for (int v = 0; v < h_.variant_count; ++v) {
        const Variant& x = h_.variants[v];
        if (!fits(x.first_off, uint64_t(h_.node_count) + 1, 4) ||
            !fits(x.edges_off, x.edge_count, sizeof(Edge)))
            return OpenResult::BadLayout;
    }
    open_ = true;
    return OpenResult::Ok;
}

int Graph::variant(uint32_t flags) const {
    for (int v = 0; v < h_.variant_count; ++v)
        if (h_.variants[v].flags == flags) return v;
    return -1;
}

bool Graph::node(uint32_t i, Point& out) {
    return i < h_.node_count && read(h_.nodes_off + i * uint32_t(sizeof(Point)), &out, sizeof out);
}

bool Graph::attr(uint32_t i, Attr& out) {
    return i < h_.attr_count && read(h_.attrs_off + i * uint32_t(sizeof(Attr)), &out, sizeof out);
}

bool Graph::geom(uint32_t i, Point& out) {
    return i < h_.geom_count && read(h_.geom_off + i * uint32_t(sizeof(Point)), &out, sizeof out);
}

bool Graph::poi(uint32_t i, Poi& out) {
    return i < h_.poi_count && read(h_.pois_off + i * uint32_t(sizeof(Poi)), &out, sizeof out);
}

void Graph::name(uint32_t off, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    size_t n = 0;
    while (n + 1 < cap && off + n < h_.names_bytes) {
        char c;
        if (!read(h_.names_off + off + uint32_t(n), &c, 1)) { out[0] = '\0'; return; }
        if (c == '\0') break;
        out[n++] = c;
    }
    out[n] = '\0';
}

bool Graph::edge_range(int v, uint32_t n, uint32_t& first, uint32_t& last) {
    if (v < 0 || v >= h_.variant_count || n >= h_.node_count) return false;
    uint32_t r[2];
    if (!read(h_.variants[v].first_off + n * 4u, r, sizeof r)) return false;
    first = r[0];
    last = r[1];
    return first <= last && last <= h_.variants[v].edge_count;
}

bool Graph::edge(int v, uint32_t i, Edge& out) {
    return v >= 0 && v < h_.variant_count && i < h_.variants[v].edge_count &&
           read(h_.variants[v].edges_off + i * uint32_t(sizeof(Edge)), &out, sizeof out);
}

}  // namespace core::route
