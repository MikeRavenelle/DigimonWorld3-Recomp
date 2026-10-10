#include <gtest/gtest.h>

#include "psxdisc/disc.h"

using namespace psxdisc;

TEST(Cue, ParsesSingleTrackMode2) {
    auto cue = parse_cue("FILE \"Digimon World 3 (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    ASSERT_TRUE(cue);
    EXPECT_EQ(cue->bin_file, "Digimon World 3 (USA).bin");
    ASSERT_EQ(cue->tracks.size(), 1u);
    EXPECT_EQ(cue->tracks[0].number, 1);
    EXPECT_EQ(cue->tracks[0].mode, "MODE2/2352");
    EXPECT_EQ(cue->tracks[0].index1_lba, 0u);
}

TEST(Cue, ParsesIndexOffset) {
    auto cue = parse_cue("FILE \"a.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 01:02:03\n");
    ASSERT_TRUE(cue);
    ASSERT_EQ(cue->tracks.size(), 2u);
    EXPECT_EQ(cue->tracks[1].index1_lba, (1u * 60 + 2) * 75 + 3);
}

TEST(Cue, RejectsMissingFile) { EXPECT_FALSE(parse_cue("TRACK 01 MODE2/2352\n")); }
TEST(Cue, RejectsMultiFile) { EXPECT_FALSE(parse_cue("FILE \"a\" BINARY\nTRACK 01 MODE2/2352\nFILE \"b\" BINARY\n")); }
TEST(Cue, CaseInsensitiveCommands) { EXPECT_TRUE(parse_cue("file \"a.bin\" BINARY\ntrack 01 MODE2/2352\n")); }

TEST(Msf, Bcd) {
    EXPECT_EQ(to_bcd(42), 0x42);
    EXPECT_EQ(from_bcd(0x59), 59);
}

TEST(Msf, RoundTrip) {
    for (uint32_t lba : {0u, 1u, 16u, 74u, 75u, 4499u, 4500u, 275317u}) {
        uint8_t m, s, f;
        lba_to_msf(lba, m, s, f);
        EXPECT_EQ(msf_to_lba(m, s, f), lba) << lba;
    }
}

TEST(Msf, LbaZeroIsTwoSeconds) {
    uint8_t m, s, f;
    lba_to_msf(0, m, s, f);
    EXPECT_EQ(m, 0x00);
    EXPECT_EQ(s, 0x02);
    EXPECT_EQ(f, 0x00);
}
