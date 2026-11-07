// Reading an .rgf through a block cache (docs/navigation.md §5).
//
// The file is 50+ MB on the card; a query touches a few thousand records
// scattered through it. The cache holds 4 kB blocks in caller-provided
// memory (PSRAM on the dash), four-way set associative, so a record costs
// a card read only the first time its block is needed.
//
// No allocation, no ESP header: the dash gives it the card, the host tests
// a byte array, and the builder's self-check a file.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/route/rgf.hpp"

namespace core::route {

// Where the bytes come from. False on a read error; the reader reports it
// and the query fails, it does not retry (rule 5).
class IBlockSource {
public:
    virtual ~IBlockSource() = default;
    virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
    virtual uint32_t size() const = 0;
};

inline constexpr uint32_t kBlockBytes = 4096;
inline constexpr uint32_t kWays       = 4;

class BlockCache {
public:
    // `blocks` is a multiple of kWays. `tags` and `stamps` hold `blocks`
    // entries, `data` blocks * kBlockBytes bytes.
    BlockCache(uint32_t* tags, uint32_t* stamps, uint8_t* data, uint32_t blocks);

    void attach(IBlockSource* src);   // forgets every cached block
    // Copies [offset, offset+len) out, through the cache. False on a read
    // error or past the end of the source.
    bool read(uint32_t offset, void* dst, uint32_t len);

    uint32_t misses() const { return misses_; }
    uint32_t hits() const { return hits_; }
    uint32_t errors() const { return errors_; }   // failed card reads, ever
    void reset_counts() { misses_ = hits_ = 0; }

private:
    const uint8_t* block(uint32_t index);

    IBlockSource* src_ = nullptr;
    uint32_t* tags_;
    uint32_t* stamps_;
    uint8_t*  data_;
    uint32_t  sets_;
    uint32_t  clock_ = 0;
    uint32_t  misses_ = 0;
    uint32_t  hits_ = 0;
    uint32_t  errors_ = 0;
};

// Static storage for a cache of N blocks; put it in PSRAM on the dash.
template <uint32_t N>
struct BlockCacheStorage {
    static_assert(N % kWays == 0, "whole sets only");
    uint32_t tags[N];
    uint32_t stamps[N];
    uint8_t  data[N * kBlockBytes];
    BlockCache make() { return BlockCache(tags, stamps, data, N); }
};

enum class OpenResult : uint8_t { Ok, ReadError, BadMagic, BadVersion, BadLayout };

// Whether the first bytes of a file are an .rgf this firmware reads, and
// the date of its OSM data. For an upload, to refuse the wrong file before
// 100 MB of it go to the card.
bool map_header_ok(const void* first, size_t n, uint32_t* osm_date = nullptr);
const char* to_string(OpenResult r);

class Graph {
public:
    explicit Graph(BlockCache& cache) : cache_(cache) {}

    OpenResult open(IBlockSource& src);
    bool is_open() const { return open_; }
    const Header& header() const { return h_; }

    // The variant index whose flags match, or -1.
    int variant(uint32_t flags) const;

    bool node(uint32_t i, Point& out);
    bool attr(uint32_t i, Attr& out);
    bool geom(uint32_t i, Point& out);
    bool poi(uint32_t i, Poi& out);
    // Copies a NUL-terminated name, cut to fit `cap`. "" if unreadable.
    void name(uint32_t off, char* out, size_t cap);

    // The edges stored at node `n` in variant `v`: [first, last).
    bool edge_range(int v, uint32_t n, uint32_t& first, uint32_t& last);
    bool edge(int v, uint32_t i, Edge& out);

    // The Attr indices listed in the grid cell that holds (lat, lon), and
    // the cell's neighbours `ring` cells out. Calls `fn(attr_index)`;
    // stops early if `fn` returns false. False on a read error.
    template <typename Fn>
    bool near(int32_t lat_e7, int32_t lon_e7, int ring, Fn&& fn);

    BlockCache& cache() { return cache_; }

private:
    bool read(uint32_t off, void* dst, uint32_t len) { return cache_.read(off, dst, len); }

    BlockCache& cache_;
    Header h_{};
    bool open_ = false;
};

template <typename Fn>
bool Graph::near(int32_t lat_e7, int32_t lon_e7, int ring, Fn&& fn) {
    if (!open_ || h_.cell_e7 == 0) return false;
    const int64_t cy = (int64_t(lat_e7) - h_.grid_lat0_e7) / h_.cell_e7;
    const int64_t cx = (int64_t(lon_e7) - h_.grid_lon0_e7) / h_.cell_e7;
    for (int64_t y = cy - ring; y <= cy + ring; ++y) {
        if (y < 0 || y >= h_.grid_h) continue;
        for (int64_t x = cx - ring; x <= cx + ring; ++x) {
            if (x < 0 || x >= h_.grid_w) continue;
            const uint32_t cell = static_cast<uint32_t>(y * h_.grid_w + x);
            uint32_t range[2];
            if (!read(h_.grid_off + cell * 4u, range, sizeof range)) return false;
            for (uint32_t k = range[0]; k < range[1]; ++k) {
                uint32_t a;
                if (!read(h_.cells_off + k * 4u, &a, 4)) return false;
                if (!fn(a)) return true;
            }
        }
    }
    return true;
}

}  // namespace core::route
