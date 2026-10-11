#include "dw3/disc_ref.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace dw3 {

namespace {

constexpr std::size_t kWindow = 32;           // bytes covered by the rolling hash
constexpr std::size_t kMinCopy = 48;          // shorter matches stay literal
constexpr std::size_t kMaxPerKey = 4;         // offsets kept per key
constexpr std::size_t kMaxAnchors = 8u << 20; // bound on index growth for degenerate streams
constexpr std::size_t kScanChunk = 1u << 20;
constexpr std::size_t kCompareBlock = 16384;
constexpr std::size_t kSampleBlocks = 16;
constexpr std::size_t kSampleBytes = 4096;

constexpr uint64_t kBase = 0x100000001B3ull;

// t[b] = b * kBase^kWindow, the contribution of a byte leaving the window.
struct OutTable {
    std::array<uint64_t, 256> t{};
    constexpr OutTable() {
        uint64_t p = 1;
        for (std::size_t i = 0; i < kWindow; ++i) p *= kBase;
        for (std::size_t b = 0; b < 256; ++b) t[b] = b * p;
    }
};
constexpr OutTable kOut;

inline uint64_t mix(uint64_t h) { return h * 0x9E3779B97F4A7C15ull; }
inline bool is_anchor(uint64_t mixed) { return (mixed >> 55) == 0; } // 9 bits: about 1 in 512
inline uint32_t key_of(uint64_t mixed) { return static_cast<uint32_t>(mixed >> 20); }

// Cache file: magic, version, reserved, stream size, sample hash, entry count, then keys (u32) and offsets (u64).
constexpr char kMagic[8] = {'D', 'W', '3', 'D', 'I', 'D', 'X', '\0'};
constexpr uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 8 + 8 + 8;
constexpr uint64_t kMaxCacheEntries = 1ull << 26;
static_assert(std::endian::native == std::endian::little, "cache file is stored little-endian");

uint64_t fnv(uint64_t h, const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Hash of the stream size and a few evenly spaced blocks; false if one cannot be read.
bool sample_hash(DiscStream& disc, uint64_t& out) {
    uint64_t size = disc.size();
    uint64_t h = fnv(0xCBF29CE484222325ull, reinterpret_cast<const uint8_t*>(&size), sizeof size);
    if (size == 0) {
        out = h;
        return true;
    }
    std::size_t blk = static_cast<std::size_t>(std::min<uint64_t>(kSampleBytes, size));
    std::vector<uint8_t> buf(blk);
    for (std::size_t i = 0; i < kSampleBlocks; ++i) {
        uint64_t off = (size - blk) / (kSampleBlocks - 1) * i;
        if (!disc.read(off, buf.data(), blk)) return false;
        h = fnv(h, buf.data(), blk);
    }
    out = h;
    return true;
}

template <typename T> void put_le(std::vector<uint8_t>& v, T x) {
    auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof x);
}

} // namespace

namespace disc_ref {

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

bool get_varint(std::span<const uint8_t> in, std::size_t& pos, uint64_t& v) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (pos >= in.size()) return false;
        uint8_t b = in[pos++];
        if (i == 9 && b > 1) return false; // would overflow 64 bits
        r |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            v = r;
            return true;
        }
    }
    return false;
}

} // namespace disc_ref

bool DiscIndex::build() {
    const uint64_t size = disc_->size();
    struct Entry {
        uint32_t key;
        uint64_t off;
    };
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(size / 400) + 16);

    // The first kWindow bytes of buf carry the end of the previous chunk so the outgoing byte is always at hand.
    std::vector<uint8_t> buf(kWindow + kScanChunk, 0);
    uint64_t h = 0, prev = 0;
    for (uint64_t base = 0; base < size;) {
        std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(kScanChunk, size - base));
        if (!disc_->read(base, buf.data() + kWindow, n)) return false;
        const uint8_t* in = buf.data() + kWindow;
        const uint8_t* out = buf.data();
        for (std::size_t i = 0; i < n; ++i) {
            uint64_t a = base + i;
            prev = h;
            h = h * kBase + in[i];
            if (a >= kWindow) h -= kOut.t[out[i]];
            if (a + 1 < kWindow || h == prev) continue; // window not full yet, or a constant run
            uint64_t m = mix(h);
            if (is_anchor(m) && entries.size() < kMaxAnchors) entries.push_back({key_of(m), a + 1 - kWindow});
        }
        std::memmove(buf.data(), buf.data() + n, kWindow);
        base += n;
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
        return x.key != y.key ? x.key < y.key : x.off < y.off;
    });
    keys_.clear();
    offs_.clear();
    keys_.reserve(entries.size());
    offs_.reserve(entries.size());
    std::size_t run = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        run = (i > 0 && entries[i].key == entries[i - 1].key) ? run + 1 : 1;
        if (run > kMaxPerKey) continue;
        keys_.push_back(entries[i].key);
        offs_.push_back(entries[i].off);
    }
    return true;
}

bool DiscIndex::load(const std::string& path, uint64_t sample) {
    std::error_code ec;
    uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < kHeaderSize) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::array<uint8_t, kHeaderSize> hdr{};
    if (!f.read(reinterpret_cast<char*>(hdr.data()), hdr.size())) return false;
    if (std::memcmp(hdr.data(), kMagic, 8) != 0) return false;
    uint32_t version;
    uint64_t stream_size, hash, count;
    std::memcpy(&version, &hdr[8], 4);
    std::memcpy(&stream_size, &hdr[16], 8);
    std::memcpy(&hash, &hdr[24], 8);
    std::memcpy(&count, &hdr[32], 8);
    if (version != kVersion || stream_size != disc_->size() || hash != sample) return false;
    if (count > kMaxCacheEntries || file_size != kHeaderSize + count * 12) return false;

    std::vector<uint32_t> keys(static_cast<std::size_t>(count));
    std::vector<uint64_t> offs(static_cast<std::size_t>(count));
    if (count) {
        if (!f.read(reinterpret_cast<char*>(keys.data()), static_cast<std::streamsize>(count * 4))) return false;
        if (!f.read(reinterpret_cast<char*>(offs.data()), static_cast<std::streamsize>(count * 8))) return false;
    }
    if (!std::is_sorted(keys.begin(), keys.end())) return false;
    for (uint64_t o : offs)
        if (o >= stream_size) return false;
    keys_ = std::move(keys);
    offs_ = std::move(offs);
    return true;
}

void DiscIndex::save(const std::string& path, uint64_t sample) const {
    std::vector<uint8_t> hdr;
    hdr.insert(hdr.end(), kMagic, kMagic + 8);
    put_le<uint32_t>(hdr, kVersion);
    put_le<uint32_t>(hdr, 0);
    put_le<uint64_t>(hdr, disc_->size());
    put_le<uint64_t>(hdr, sample);
    put_le<uint64_t>(hdr, keys_.size());

    // Write next to the target and rename, so a concurrent reader never sees a partial file.
    std::random_device rd;
    std::string tmp = path + ".tmp" + std::to_string(rd()) + std::to_string(rd());
    bool ok;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        f.write(reinterpret_cast<const char*>(keys_.data()), static_cast<std::streamsize>(keys_.size() * 4));
        f.write(reinterpret_cast<const char*>(offs_.data()), static_cast<std::streamsize>(offs_.size() * 8));
        f.flush();
        ok = static_cast<bool>(f);
    }
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) std::filesystem::remove(tmp, ec);
}

std::unique_ptr<DiscIndex> DiscIndex::open(DiscStream& disc, const std::string& cache_path) {
    std::unique_ptr<DiscIndex> idx(new DiscIndex(disc));
    uint64_t sample = 0;
    if (!sample_hash(disc, sample)) return nullptr;
    if (!cache_path.empty() && idx->load(cache_path, sample)) return idx;
    if (!idx->build()) return nullptr;
    if (!cache_path.empty()) idx->save(cache_path, sample);
    return idx;
}

std::vector<uint8_t> DiscIndex::encode(std::span<const uint8_t> data) const {
    std::vector<uint8_t> out;
    const std::size_t n = data.size();
    if (n == 0) return out;
    const uint64_t disc_size = disc_->size();
    std::vector<uint8_t> scratch;

    // Length of the common prefix of data[p..] and the stream at s, over at most `max` bytes.
    auto match_forward = [&](std::size_t p, uint64_t s, std::size_t max) {
        std::size_t len = 0;
        while (len < max) {
            std::size_t blk = std::min(kCompareBlock, max - len);
            scratch.resize(blk);
            if (!disc_->read(s + len, scratch.data(), blk)) break;
            std::size_t k = 0;
            while (k < blk && scratch[k] == data[p + len + k]) ++k;
            len += k;
            if (k < blk) break;
        }
        return len;
    };
    // Number of bytes just before data[p] and before stream offset s that agree, over at most `max` bytes.
    auto match_backward = [&](std::size_t p, uint64_t s, std::size_t max) {
        std::size_t len = 0;
        while (len < max) {
            std::size_t blk = std::min(kCompareBlock, max - len);
            scratch.resize(blk);
            if (!disc_->read(s - len - blk, scratch.data(), blk)) break;
            std::size_t k = 0;
            while (k < blk && scratch[blk - 1 - k] == data[p - len - 1 - k]) ++k;
            len += k;
            if (k < blk) break;
        }
        return len;
    };

    std::size_t lit = 0; // start of the literals not yet written
    std::size_t p = 0;   // window start
    uint64_t h = 0, prev = 0;
    bool have_prev = false;
    auto restart = [&] {
        h = 0;
        for (std::size_t k = 0; k < kWindow; ++k) h = h * kBase + data[p + k];
        have_prev = false;
    };
    if (n >= kWindow) {
        restart();
        for (;;) {
            uint64_t m = mix(h);
            if (is_anchor(m) && !(have_prev && h == prev)) {
                uint32_t key = key_of(m);
                std::size_t best_back = 0, best_len = 0;
                uint64_t best_off = 0;
                auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
                for (; it != keys_.end() && *it == key; ++it) {
                    uint64_t s = offs_[static_cast<std::size_t>(it - keys_.begin())];
                    std::size_t fwd =
                        match_forward(p, s, static_cast<std::size_t>(std::min<uint64_t>(n - p, disc_size - s)));
                    if (fwd < kWindow) continue; // hash collision
                    std::size_t back = match_backward(p, s, static_cast<std::size_t>(std::min<uint64_t>(p - lit, s)));
                    if (back + fwd > best_len) {
                        best_len = back + fwd;
                        best_back = back;
                        best_off = s - back;
                    }
                }
                if (best_len >= kMinCopy) {
                    std::size_t start = p - best_back;
                    disc_ref::put_varint(out, start - lit);
                    out.insert(out.end(), data.begin() + lit, data.begin() + start);
                    disc_ref::put_varint(out, best_len);
                    disc_ref::put_varint(out, best_off);
                    p = start + best_len;
                    lit = p;
                    if (p + kWindow > n) break;
                    restart();
                    continue;
                }
            }
            if (p + kWindow >= n) break;
            prev = h;
            have_prev = true;
            h = h * kBase + data[p + kWindow] - kOut.t[data[p]];
            ++p;
        }
    }
    if (lit < n) {
        disc_ref::put_varint(out, n - lit);
        out.insert(out.end(), data.begin() + lit, data.end());
        out.push_back(0);
    }
    return out;
}

bool DiscIndex::decode(DiscStream& disc, std::span<const uint8_t> in, uint64_t raw_size, std::vector<uint8_t>& out,
                       std::string& error) {
    out.clear();
    auto fail = [&](const char* msg) {
        error = msg;
        out.clear();
        return false;
    };
    const uint64_t disc_size = disc.size();
    out.reserve(static_cast<std::size_t>(std::min<uint64_t>(raw_size, 64u << 20)));
    std::size_t pos = 0;
    while (pos < in.size()) {
        uint64_t lits, copy;
        if (!disc_ref::get_varint(in, pos, lits)) return fail("bad literal count");
        if (lits > in.size() - pos) return fail("truncated literals");
        if (lits > raw_size - out.size()) return fail("literals exceed raw size");
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + static_cast<std::size_t>(lits));
        pos += static_cast<std::size_t>(lits);
        if (!disc_ref::get_varint(in, pos, copy)) return fail("bad copy length");
        if (copy == 0) continue;
        uint64_t off;
        if (!disc_ref::get_varint(in, pos, off)) return fail("bad copy offset");
        if (copy > raw_size - out.size()) return fail("copy exceeds raw size");
        if (off > disc_size || copy > disc_size - off) return fail("copy outside the disc");
        std::size_t at = out.size();
        out.resize(at + static_cast<std::size_t>(copy));
        if (!disc.read(off, out.data() + at, static_cast<std::size_t>(copy))) return fail("disc read failed");
    }
    if (out.size() != raw_size) return fail("decoded size mismatch");
    return true;
}

} // namespace dw3
