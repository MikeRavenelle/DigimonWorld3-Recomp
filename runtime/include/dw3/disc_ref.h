#pragma once

// Disc references for save states. Most of RAM and SPU RAM is game data copied verbatim from the disc, so a state
// buffer is rewritten as a sequence of literals and references into the disc's byte stream before compression.
//
// Encoded format (little-endian, unsigned LEB128 varints of at most 10 bytes), a sequence of ops until the input ends:
//   varint L             literal byte count (may be 0)
//   L bytes              the literals
//   varint C             copy length (0 = no copy)
//   varint offset        only when C > 0: absolute byte offset into the disc stream
// A copy appends C bytes read from the stream at that offset. The final op may have C = 0.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dw3 {

// The disc as one byte stream: the 2048 user-data bytes of each sector, sector after sector.
class DiscStream {
public:
    virtual ~DiscStream() = default;
    virtual uint64_t size() const = 0;                                   // sector count * 2048
    virtual bool read(uint64_t offset, uint8_t* out, std::size_t n) = 0; // false on failure / out of range
};

// Finds stretches of a buffer that also occur in the disc stream. Content-defined anchors (about one per 512 bytes of
// the stream, chosen by a 32-byte rolling hash) are indexed by hash, so the alignment of the data does not matter.
class DiscIndex {
public:
    // Loads the cached index from `cache_path` when it exists and belongs to this stream (same size and same hash of
    // a few sampled blocks), otherwise scans the whole stream and writes the cache. Returns null only if the stream
    // cannot be read. An empty `cache_path` disables caching. The stream must outlive the returned index.
    static std::unique_ptr<DiscIndex> open(DiscStream& disc, const std::string& cache_path);

    // Deterministic for a given index and input.
    std::vector<uint8_t> encode(std::span<const uint8_t> data) const;

    // Reverses encode() and needs no index. `raw_size` is the expected decoded size. Returns false and sets `error`
    // on malformed input, reads outside the stream or a size mismatch.
    static bool decode(DiscStream& disc, std::span<const uint8_t> encoded, uint64_t raw_size, std::vector<uint8_t>& out,
                       std::string& error);

    std::size_t anchor_count() const { return keys_.size(); }

private:
    explicit DiscIndex(DiscStream& disc) : disc_(&disc) {}
    bool build();
    bool load(const std::string& path, uint64_t sample_hash);
    void save(const std::string& path, uint64_t sample_hash) const;

    DiscStream* disc_;
    std::vector<uint32_t> keys_; // sorted; equal keys are adjacent
    std::vector<uint64_t> offs_; // offs_[i] belongs to keys_[i]
};

namespace disc_ref {

void put_varint(std::vector<uint8_t>& out, uint64_t v);
// Reads a varint at `pos` and advances it. False on truncation or a value that does not fit in 64 bits.
bool get_varint(std::span<const uint8_t> in, std::size_t& pos, uint64_t& v);

} // namespace disc_ref

} // namespace dw3
