#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "psxdisc/iso9660.h"

using namespace psxdisc;

namespace {

void put32(std::vector<uint8_t>& v, std::size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}

// Appends an ISO9660 directory record.
void add_record(std::vector<uint8_t>& sec, std::size_t& pos, const std::string& name, uint32_t lba, uint32_t size,
                bool dir) {
    std::size_t len = 33 + name.size();
    if (len & 1) ++len;
    sec[pos] = static_cast<uint8_t>(len);
    put32(sec, pos + 2, lba);
    put32(sec, pos + 10, size);
    sec[pos + 25] = dir ? 2 : 0;
    sec[pos + 32] = static_cast<uint8_t>(name.size());
    std::copy(name.begin(), name.end(), sec.begin() + pos + 33);
    pos += len;
}

}  // namespace

TEST(Iso9660, ParsesRecordsAndSkipsDotEntries) {
    std::vector<uint8_t> sec(2048, 0);
    std::size_t pos = 0;
    add_record(sec, pos, std::string(1, '\0'), 20, 2048, true);
    add_record(sec, pos, std::string(1, '\1'), 18, 2048, true);
    add_record(sec, pos, "SYSTEM.CNF;1", 23, 70, false);
    add_record(sec, pos, "PRO", 474, 2048, true);

    auto recs = parse_directory(sec);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].name, "SYSTEM.CNF");
    EXPECT_EQ(recs[0].lba, 23u);
    EXPECT_EQ(recs[0].size, 70u);
    EXPECT_FALSE(recs[0].is_dir);
    EXPECT_EQ(recs[1].name, "PRO");
    EXPECT_TRUE(recs[1].is_dir);
}

TEST(Iso9660, ContinuesAcrossSectorPadding) {
    std::vector<uint8_t> data(4096, 0);
    std::size_t pos = 0;
    add_record(data, pos, "A.BIN;1", 100, 10, false);
    pos = 2048;  // rest of sector 0 is zero padding
    add_record(data, pos, "B.BIN;1", 101, 20, false);
    auto recs = parse_directory(data);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[1].name, "B.BIN");
}

TEST(Iso9660, StopsOnTruncatedRecord) {
    std::vector<uint8_t> data(40, 0);
    data[0] = 60;  // claims to be longer than the buffer
    EXPECT_TRUE(parse_directory(data).empty());
}

TEST(Iso9660, EmptyInput) { EXPECT_TRUE(parse_directory({}).empty()); }
