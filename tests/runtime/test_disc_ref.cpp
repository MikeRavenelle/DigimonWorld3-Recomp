#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dw3/disc_ref.h"

using dw3::DiscIndex;
using dw3::DiscStream;
using Bytes = std::vector<uint8_t>;
namespace dr = dw3::disc_ref;

namespace {

// Deterministic xorshift byte generator.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    Bytes bytes(std::size_t n) {
        Bytes b(n);
        for (auto& x : b) x = static_cast<uint8_t>(next() >> 24);
        return b;
    }
};

class FakeStream : public DiscStream {
public:
    explicit FakeStream(Bytes d) : data(std::move(d)) {}
    uint64_t size() const override { return data.size(); }
    bool read(uint64_t offset, uint8_t* out, std::size_t n) override {
        ++reads;
        if (fail || offset > data.size() || n > data.size() - offset) return false;
        std::memcpy(out, data.data() + offset, n);
        return true;
    }
    Bytes data;
    int reads = 0;
    bool fail = false;
};

constexpr std::size_t kStreamSize = 4u << 20;

FakeStream& shared_stream() {
    static FakeStream s = [] {
        Rng r(1);
        Bytes b = r.bytes(kStreamSize);
        // Second half written separately, so a stretch can cross the seam.
        Rng r2(2);
        Bytes tail = r2.bytes(kStreamSize / 2);
        std::copy(tail.begin(), tail.end(), b.begin() + kStreamSize / 2);
        return FakeStream(std::move(b));
    }();
    return s;
}

const DiscIndex& shared_index() {
    static std::unique_ptr<DiscIndex> idx = DiscIndex::open(shared_stream(), "");
    return *idx;
}

Bytes round_trip(const Bytes& data, Bytes* encoded_out = nullptr) {
    Bytes enc = shared_index().encode(data);
    Bytes dec;
    std::string err;
    EXPECT_TRUE(DiscIndex::decode(shared_stream(), enc, data.size(), dec, err)) << err;
    if (encoded_out) *encoded_out = enc;
    return dec;
}

void append_from_stream(Bytes& out, uint64_t off, std::size_t n) {
    const Bytes& d = shared_stream().data;
    out.insert(out.end(), d.begin() + off, d.begin() + off + n);
}

std::string temp_file(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("dw3_disc_ref_") + name)).string();
}

} // namespace

TEST(DiscRef, VarintRoundTripAndEdges) {
    const uint64_t values[] = {0, 1, 127, 128, 255, 16383, 16384, 0xFFFFFFFFull, 1ull << 35, ~0ull};
    for (uint64_t v : values) {
        Bytes b;
        dr::put_varint(b, v);
        std::size_t pos = 0;
        uint64_t got = 0;
        ASSERT_TRUE(dr::get_varint(b, pos, got));
        EXPECT_EQ(got, v);
        EXPECT_EQ(pos, b.size());
    }
    Bytes b;
    dr::put_varint(b, 127);
    EXPECT_EQ(b, Bytes({0x7F}));
    b.clear();
    dr::put_varint(b, 128);
    EXPECT_EQ(b, Bytes({0x80, 0x01}));
    b.clear();
    dr::put_varint(b, ~0ull);
    EXPECT_EQ(b.size(), 10u);
}

TEST(DiscRef, VarintRejectsBadInput) {
    uint64_t v;
    std::size_t pos = 0;
    EXPECT_FALSE(dr::get_varint(Bytes{}, pos, v));
    pos = 0;
    EXPECT_FALSE(dr::get_varint(Bytes{0x80}, pos, v)); // truncated
    pos = 0;
    EXPECT_FALSE(dr::get_varint(Bytes(11, 0x80), pos, v)); // too long
    pos = 0;
    Bytes overflow(9, 0xFF);
    overflow.push_back(0x02); // bit 64 set
    EXPECT_FALSE(dr::get_varint(overflow, pos, v));
}

TEST(DiscRef, EmptyAndTinyInputs) {
    EXPECT_TRUE(shared_index().encode({}).empty());
    EXPECT_TRUE(round_trip({}).empty());
    for (std::size_t n : {1u, 2u, 31u, 32u, 33u, 47u, 48u, 49u}) {
        Bytes d;
        append_from_stream(d, 12345, n);
        EXPECT_EQ(round_trip(d), d) << n;
        Rng r(n);
        Bytes rnd = r.bytes(n);
        EXPECT_EQ(round_trip(rnd), rnd) << n;
    }
}

TEST(DiscRef, MixedDataRoundTripsAndShrinks) {
    Rng r(7);
    Bytes data;
    // 70% stream copies at assorted alignments, the rest random.
    const std::size_t copy_total = 700 * 1024;
    std::size_t copied = 0;
    while (copied < copy_total) {
        std::size_t len = 2000 + r.next() % 6000;
        uint64_t off = r.next() % (kStreamSize - len);
        append_from_stream(data, off, len);
        copied += len;
        Bytes noise = r.bytes(static_cast<std::size_t>(len * 3 / 7));
        data.insert(data.end(), noise.begin(), noise.end());
    }
    Bytes enc;
    EXPECT_EQ(round_trip(data, &enc), data);
    EXPECT_LT(enc.size(), data.size() / 2);
}

TEST(DiscRef, StretchCrossingSeam) {
    Rng r(9);
    Bytes data = r.bytes(100);
    append_from_stream(data, kStreamSize / 2 - 3000, 6000);
    Bytes tail = r.bytes(100);
    data.insert(data.end(), tail.begin(), tail.end());
    Bytes enc;
    EXPECT_EQ(round_trip(data, &enc), data);
    EXPECT_LT(enc.size(), data.size() / 2);
}

TEST(DiscRef, BackwardExtensionReclaimsLiterals) {
    // One long copy: the first anchor may sit deep inside it, the copy must still start at the beginning.
    Bytes data;
    append_from_stream(data, 777777, 20000);
    Bytes enc;
    EXPECT_EQ(round_trip(data, &enc), data);
    EXPECT_LT(enc.size(), 32u);
}

TEST(DiscRef, NoMatchHasSmallOverhead) {
    Rng r(11);
    Bytes data = r.bytes(300000);
    Bytes enc;
    EXPECT_EQ(round_trip(data, &enc), data);
    EXPECT_LE(enc.size(), data.size() + 16);
}

TEST(DiscRef, ConstantRunsRoundTrip) {
    Bytes data(100000, 0);
    EXPECT_EQ(round_trip(data), data);
    Bytes pattern(50000);
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i % 3);
    EXPECT_EQ(round_trip(pattern), pattern);
}

TEST(DiscRef, EncodeIsDeterministic) {
    Rng r(13);
    Bytes data = r.bytes(1000);
    append_from_stream(data, 5000, 9000);
    EXPECT_EQ(shared_index().encode(data), shared_index().encode(data));
}

TEST(DiscRef, CacheRoundTripAvoidsRescan) {
    std::string path = temp_file("cache.idx");
    std::filesystem::remove(path);

    FakeStream a(shared_stream().data);
    auto built = DiscIndex::open(a, path);
    ASSERT_TRUE(built);
    EXPECT_GT(a.reads, 16); // scanned the whole stream
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(built->anchor_count(), 0u);

    FakeStream b(shared_stream().data);
    auto loaded = DiscIndex::open(b, path);
    ASSERT_TRUE(loaded);
    EXPECT_LE(b.reads, 16); // only the sample blocks
    EXPECT_EQ(loaded->anchor_count(), built->anchor_count());

    Bytes data;
    append_from_stream(data, 100000, 5000);
    EXPECT_EQ(loaded->encode(data), built->encode(data));
    std::filesystem::remove(path);
}

TEST(DiscRef, CacheForDifferentStreamIsRebuilt) {
    std::string path = temp_file("other.idx");
    std::filesystem::remove(path);
    FakeStream a(shared_stream().data);
    ASSERT_TRUE(DiscIndex::open(a, path));

    // Same size, different content.
    Rng r(21);
    FakeStream b(r.bytes(kStreamSize));
    auto idx = DiscIndex::open(b, path);
    ASSERT_TRUE(idx);
    EXPECT_GT(b.reads, 16);
    Bytes data(b.data.begin() + 4000, b.data.begin() + 9000);
    Bytes enc = idx->encode(data);
    EXPECT_LT(enc.size(), 64u);

    // Different size.
    FakeStream c(Bytes(b.data.begin(), b.data.begin() + kStreamSize / 2));
    auto idx2 = DiscIndex::open(c, path);
    ASSERT_TRUE(idx2);
    EXPECT_GT(c.reads, 16);
    std::filesystem::remove(path);
}

TEST(DiscRef, CorruptCacheIsIgnored) {
    std::string path = temp_file("corrupt.idx");
    {
        std::ofstream f(path, std::ios::binary);
        f << "not an index";
    }
    Rng r(31);
    FakeStream s(r.bytes(300000));
    auto idx = DiscIndex::open(s, path);
    ASSERT_TRUE(idx);
    EXPECT_GT(s.reads, 16);
    // The rebuilt cache now loads.
    FakeStream s2(s.data);
    auto again = DiscIndex::open(s2, path);
    ASSERT_TRUE(again);
    EXPECT_LE(s2.reads, 16);
    std::filesystem::remove(path);
}

TEST(DiscRef, UnreadableStreamGivesNull) {
    FakeStream s(Bytes(5000, 1));
    s.fail = true;
    EXPECT_FALSE(DiscIndex::open(s, ""));
}

TEST(DiscRef, SmallAndEmptyStreams) {
    FakeStream empty{Bytes{}};
    auto idx = DiscIndex::open(empty, "");
    ASSERT_TRUE(idx);
    Rng r(41);
    Bytes data = r.bytes(500);
    Bytes enc = idx->encode(data);
    Bytes dec;
    std::string err;
    EXPECT_TRUE(DiscIndex::decode(empty, enc, data.size(), dec, err)) << err;
    EXPECT_EQ(dec, data);

    FakeStream tiny(r.bytes(10));
    auto idx2 = DiscIndex::open(tiny, "");
    ASSERT_TRUE(idx2);
    EXPECT_EQ(idx2->encode(data).size() > 0, true);
}

TEST(DiscRef, DecodeRejectsMalformedInput) {
    Bytes data;
    Rng r(51);
    Bytes noise = r.bytes(200);
    data.insert(data.end(), noise.begin(), noise.end());
    append_from_stream(data, 9999, 4000);
    Bytes enc = shared_index().encode(data);
    ASSERT_LT(enc.size(), data.size());

    FakeStream& s = shared_stream();
    Bytes out;
    std::string err;

    // Wrong raw sizes.
    EXPECT_FALSE(DiscIndex::decode(s, enc, data.size() + 1, out, err));
    EXPECT_FALSE(DiscIndex::decode(s, enc, data.size() - 1, out, err));
    EXPECT_FALSE(err.empty());

    // Every truncation either fails cleanly or (at an op boundary) fails on the size check.
    for (std::size_t cut = 0; cut < enc.size(); ++cut) {
        Bytes t(enc.begin(), enc.begin() + cut);
        EXPECT_FALSE(DiscIndex::decode(s, t, data.size(), out, err)) << cut;
    }

    // Offset past the end of the stream.
    Bytes bad;
    dr::put_varint(bad, 0);
    dr::put_varint(bad, 100);
    dr::put_varint(bad, kStreamSize + 1);
    EXPECT_FALSE(DiscIndex::decode(s, bad, 100, out, err));
    // Copy running off the end.
    bad.clear();
    dr::put_varint(bad, 0);
    dr::put_varint(bad, 100);
    dr::put_varint(bad, kStreamSize - 50);
    EXPECT_FALSE(DiscIndex::decode(s, bad, 100, out, err));
    // Copy longer than raw_size.
    bad.clear();
    dr::put_varint(bad, 0);
    dr::put_varint(bad, 100);
    dr::put_varint(bad, 0);
    EXPECT_FALSE(DiscIndex::decode(s, bad, 99, out, err));
    // Huge counts must not allocate or overflow.
    bad.clear();
    dr::put_varint(bad, ~0ull);
    EXPECT_FALSE(DiscIndex::decode(s, bad, 100, out, err));
    bad.clear();
    dr::put_varint(bad, 0);
    dr::put_varint(bad, ~0ull);
    dr::put_varint(bad, ~0ull);
    EXPECT_FALSE(DiscIndex::decode(s, bad, ~0ull, out, err));
    // Overlong varint.
    EXPECT_FALSE(DiscIndex::decode(s, Bytes(12, 0xFF), 10, out, err));
    // Empty input only decodes to nothing.
    EXPECT_TRUE(DiscIndex::decode(s, {}, 0, out, err));
    EXPECT_FALSE(DiscIndex::decode(s, {}, 1, out, err));
}

TEST(DiscRef, DecodeReportsDiscReadFailure) {
    FakeStream s(shared_stream().data);
    Bytes enc;
    dr::put_varint(enc, 0);
    dr::put_varint(enc, 64);
    dr::put_varint(enc, 1000);
    Bytes out;
    std::string err;
    EXPECT_TRUE(DiscIndex::decode(s, enc, 64, out, err));
    s.fail = true;
    EXPECT_FALSE(DiscIndex::decode(s, enc, 64, out, err));
}
