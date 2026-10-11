// Memory card image format, filesystem layer and .mcd file helpers.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "dw3/memcard.h"

namespace {

using namespace dw3::memcard;
using Image = std::vector<uint8_t>;

Image blank() {
    Image img(kCardSize, 0x77);
    format_memory_card(std::span<uint8_t, kCardSize>(img.data(), kCardSize));
    return img;
}

std::span<const uint8_t> frame(const Image& img, std::size_t n) { return {img.data() + n * kFrameSize, kFrameSize}; }

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }

Image pattern(std::size_t n, uint8_t seed = 1) {
    Image v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
    return v;
}

std::filesystem::path temp_path(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("dw3_memcard_test_" + name);
    std::filesystem::remove(p);
    return p;
}

// ---- Format ---------------------------------------------------------------------------------

TEST(MemcardFormat, HeaderFrame) {
    Image img = blank();
    EXPECT_EQ(img[0], 'M');
    EXPECT_EQ(img[1], 'C');
    for (int i = 2; i < 127; ++i) EXPECT_EQ(img[i], 0) << i;
    EXPECT_EQ(img[127], 'M' ^ 'C');
}

TEST(MemcardFormat, WriteTestFrameCopiesHeader) {
    Image img = blank();
    EXPECT_TRUE(std::equal(img.begin(), img.begin() + 128, img.begin() + 63 * 128));
}

TEST(MemcardFormat, DirectoryFramesAreFree) {
    Image img = blank();
    for (std::size_t n = 1; n <= 15; ++n) {
        auto f = frame(img, n);
        EXPECT_EQ(f[0], 0xA0) << n;
        EXPECT_EQ(le32(&f[4]), 0u) << n;
        EXPECT_EQ(f[8], 0xFF) << n;
        EXPECT_EQ(f[9], 0xFF) << n;
        EXPECT_TRUE(frame_checksum_ok(f)) << n;
    }
}

TEST(MemcardFormat, FreeEntryChecksumValue) {
    Image img = blank();
    EXPECT_EQ(frame(img, 1)[127], 0xA0 ^ 0xFF ^ 0xFF);
}

TEST(MemcardFormat, BrokenSectorListIsEmpty) {
    Image img = blank();
    for (std::size_t n = 16; n <= 35; ++n) {
        auto f = frame(img, n);
        EXPECT_EQ(le32(&f[0]), 0xFFFFFFFFu) << n;
        EXPECT_TRUE(frame_checksum_ok(f)) << n;
    }
}

TEST(MemcardFormat, DataAreaIsZeroed) {
    Image img = blank();
    for (std::size_t i = 64 * kFrameSize; i < img.size(); ++i) ASSERT_EQ(img[i], 0) << i;
}

TEST(MemcardFormat, OverwritesPreviousContents) {
    Image img(kCardSize, 0xEE);
    format_memory_card(std::span<uint8_t, kCardSize>(img.data(), kCardSize));
    EXPECT_TRUE(is_formatted(img));
    EXPECT_EQ(img[100 * 128], 0);
}

TEST(MemcardFormat, FormatIsIdempotent) {
    Image a = blank(), b = blank();
    format_memory_card(std::span<uint8_t, kCardSize>(b.data(), kCardSize));
    EXPECT_EQ(a, b);
}

TEST(MemcardFormat, IsFormattedAcceptsBlank) { EXPECT_TRUE(is_formatted(blank())); }

TEST(MemcardFormat, IsFormattedRejectsZeroedImage) { EXPECT_FALSE(is_formatted(Image(kCardSize, 0))); }

TEST(MemcardFormat, IsFormattedRejectsWrongSize) {
    Image img = blank();
    img.pop_back();
    EXPECT_FALSE(is_formatted(img));
    EXPECT_FALSE(is_formatted(std::span<const uint8_t>{}));
}

TEST(MemcardFormat, IsFormattedRejectsBadSignature) {
    Image img = blank();
    img[1] = 'X';
    EXPECT_FALSE(is_formatted(img));
}

TEST(MemcardFormat, IsFormattedRejectsBadHeaderChecksum) {
    Image img = blank();
    img[127] ^= 1;
    EXPECT_FALSE(is_formatted(img));
}

TEST(MemcardFormat, IsFormattedRejectsBadDirectoryChecksum) {
    for (std::size_t n = 1; n <= 15; ++n) {
        Image img = blank();
        img[n * 128 + 127] ^= 0x10;
        EXPECT_FALSE(is_formatted(img)) << n;
    }
}

TEST(MemcardFormat, FileDataDoesNotAffectIsFormatted) {
    Image img = blank();
    for (std::size_t i = 64 * 128; i < img.size(); ++i) img[i] = 0xA5;
    EXPECT_TRUE(is_formatted(img));
}

// ---- Checksums ------------------------------------------------------------------------------

TEST(MemcardChecksum, XorOfFirst127Bytes) {
    Image f(128, 0);
    f[0] = 0x12;
    f[5] = 0x34;
    f[126] = 0x01;
    EXPECT_EQ(frame_checksum(f), 0x12 ^ 0x34 ^ 0x01);
}

TEST(MemcardChecksum, IgnoresByte127) {
    Image f(128, 0);
    f[127] = 0xFF;
    EXPECT_EQ(frame_checksum(f), 0);
}

TEST(MemcardChecksum, SealWritesChecksum) {
    Image f = pattern(128);
    seal_frame(f);
    EXPECT_TRUE(frame_checksum_ok(f));
    f[3] ^= 1;
    EXPECT_FALSE(frame_checksum_ok(f));
}

TEST(MemcardChecksum, ShortFrameIsNotOk) { EXPECT_FALSE(frame_checksum_ok(Image(64, 0))); }

// ---- Filesystem -----------------------------------------------------------------------------

TEST(MemcardFs, ValidOnBlankCard) {
    Image img = blank();
    EXPECT_TRUE(CardFs(img).valid());
}

TEST(MemcardFs, InvalidOnGarbage) {
    Image img(kCardSize, 0x55);
    CardFs fs(img);
    EXPECT_FALSE(fs.valid());
    EXPECT_EQ(fs.create("X", 1), Status::BadDevice);
    EXPECT_EQ(fs.remove("X"), Status::BadDevice);
    EXPECT_EQ(fs.rename("X", "Y"), Status::BadDevice);
}

TEST(MemcardFs, EmptyListAndFullFreeCount) {
    Image img = blank();
    CardFs fs(img);
    EXPECT_TRUE(fs.list().empty());
    EXPECT_EQ(fs.free_blocks(), 15);
}

TEST(MemcardFs, CreateSingleBlockFile) {
    Image img = blank();
    CardFs fs(img);
    ASSERT_EQ(fs.create("BASLUS-01436SAVE", 1), Status::Ok);
    auto f = fs.find("BASLUS-01436SAVE");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->size, kBlockSize);
    EXPECT_EQ(f->blocks, std::vector<uint8_t>{1});
    EXPECT_EQ(fs.free_blocks(), 14);
    EXPECT_TRUE(is_formatted(img));
}

TEST(MemcardFs, DirectoryEntryLayoutOfSingleBlock) {
    Image img = blank();
    CardFs fs(img);
    fs.create("HELLO", 1);
    auto e = frame(img, 1);
    EXPECT_EQ(e[0], 0x51);
    EXPECT_EQ(le32(&e[4]), 8192u);
    EXPECT_EQ(e[8], 0xFF);
    EXPECT_EQ(e[9], 0xFF);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(&e[10])), "HELLO");
    EXPECT_EQ(e[10 + 5], 0);
    EXPECT_TRUE(frame_checksum_ok(e));
}

TEST(MemcardFs, MultiBlockChainStatesAndLinks) {
    Image img = blank();
    CardFs fs(img);
    ASSERT_EQ(fs.create("BIG", 3), Status::Ok);
    EXPECT_EQ(frame(img, 1)[0], 0x51);
    EXPECT_EQ(frame(img, 2)[0], 0x52);
    EXPECT_EQ(frame(img, 3)[0], 0x53);
    EXPECT_EQ(frame(img, 1)[8], 1);  // next = block 2 (stored as index 1)
    EXPECT_EQ(frame(img, 2)[8], 2);
    EXPECT_EQ(frame(img, 3)[8], 0xFF);
    EXPECT_EQ(le32(&frame(img, 1)[4]), 3u * 8192u);
    for (std::size_t n = 1; n <= 3; ++n) EXPECT_TRUE(frame_checksum_ok(frame(img, n))) << n;
    EXPECT_EQ(fs.find("BIG")->blocks, (std::vector<uint8_t>{1, 2, 3}));
}

TEST(MemcardFs, CreateRejectsBadArguments) {
    Image img = blank();
    CardFs fs(img);
    EXPECT_EQ(fs.create("", 1), Status::BadArg);
    EXPECT_EQ(fs.create("A", 0), Status::BadArg);
    EXPECT_EQ(fs.create("A", 16), Status::BadArg);
    EXPECT_EQ(fs.create("A", -1), Status::BadArg);
    EXPECT_EQ(fs.create(std::string(21, 'N'), 1), Status::BadArg);
    EXPECT_EQ(fs.free_blocks(), 15);
}

TEST(MemcardFs, NameOfExactly20CharsIsAccepted) {
    Image img = blank();
    CardFs fs(img);
    std::string n(20, 'N');
    ASSERT_EQ(fs.create(n, 1), Status::Ok);
    EXPECT_EQ(fs.find(n)->name, n);
    EXPECT_EQ(fs.list().at(0).name, n);
}

TEST(MemcardFs, DuplicateNameIsRejected) {
    Image img = blank();
    CardFs fs(img);
    ASSERT_EQ(fs.create("A", 1), Status::Ok);
    EXPECT_EQ(fs.create("A", 1), Status::Exists);
    EXPECT_EQ(fs.free_blocks(), 14);
}

TEST(MemcardFs, NamesAreCaseSensitive) {
    Image img = blank();
    CardFs fs(img);
    fs.create("abc", 1);
    EXPECT_FALSE(fs.find("ABC"));
    EXPECT_EQ(fs.create("ABC", 1), Status::Ok);
}

TEST(MemcardFs, PrefixNamesAreDistinct) {
    Image img = blank();
    CardFs fs(img);
    fs.create("SAVE", 1);
    fs.create("SAVE2", 1);
    EXPECT_EQ(fs.find("SAVE")->blocks.front(), 1);
    EXPECT_EQ(fs.find("SAVE2")->blocks.front(), 2);
    EXPECT_FALSE(fs.find("SAV"));
}

TEST(MemcardFs, FindMissing) {
    Image img = blank();
    EXPECT_FALSE(CardFs(img).find("NOPE"));
}

TEST(MemcardFs, FillCardWithFifteenSingles) {
    Image img = blank();
    CardFs fs(img);
    for (int i = 0; i < 15; ++i) ASSERT_EQ(fs.create("F" + std::to_string(i), 1), Status::Ok) << i;
    EXPECT_EQ(fs.free_blocks(), 0);
    EXPECT_EQ(fs.create("EXTRA", 1), Status::NoSpace);
    EXPECT_EQ(fs.list().size(), 15u);
}

TEST(MemcardFs, FullCardSingleFile) {
    Image img = blank();
    CardFs fs(img);
    ASSERT_EQ(fs.create("ALL", 15), Status::Ok);
    EXPECT_EQ(fs.free_blocks(), 0);
    auto f = fs.find("ALL");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->blocks.size(), 15u);
    EXPECT_EQ(f->size, 15u * 8192u);
}

TEST(MemcardFs, NoSpaceDoesNotModifyCard) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 10);
    Image before = img;
    EXPECT_EQ(fs.create("B", 6), Status::NoSpace);
    EXPECT_EQ(img, before);
}

TEST(MemcardFs, ListPreservesDirectoryOrder) {
    Image img = blank();
    CardFs fs(img);
    fs.create("ONE", 1);
    fs.create("TWO", 2);
    fs.create("THREE", 1);
    auto l = fs.list();
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[0].name, "ONE");
    EXPECT_EQ(l[1].name, "TWO");
    EXPECT_EQ(l[2].name, "THREE");
    EXPECT_EQ(l[2].blocks.front(), 4);
}

TEST(MemcardFs, ListShowsOnlyFirstBlocks) {
    Image img = blank();
    CardFs fs(img);
    fs.create("CHAIN", 4);
    EXPECT_EQ(fs.list().size(), 1u);
}

TEST(MemcardFs, WriteThenReadSingleBlock) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 1);
    auto f = *fs.find("F");
    Image data = pattern(300);
    EXPECT_EQ(fs.write(f, 0, data), 300u);
    Image out(300);
    EXPECT_EQ(fs.read(f, 0, out), 300u);
    EXPECT_EQ(out, data);
}

TEST(MemcardFs, DataLandsAtBlockFrames) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.create("B", 1);
    auto b = *fs.find("B");
    Image d = pattern(128, 9);
    fs.write(b, 0, d);
    EXPECT_TRUE(std::equal(d.begin(), d.end(), img.begin() + 2 * 8192));  // block 2 = frames 128..191
}

TEST(MemcardFs, OffsetAccessWithinBlock) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 1);
    auto f = *fs.find("F");
    Image d = pattern(128, 3);
    fs.write(f, 1280, d);  // frame 10
    Image out(128);
    EXPECT_EQ(fs.read(f, 1280, out), 128u);
    EXPECT_EQ(out, d);
    EXPECT_TRUE(std::equal(d.begin(), d.end(), img.begin() + 8192 + 1280));
}

TEST(MemcardFs, ReadClampsToFileSize) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 1);
    auto f = *fs.find("F");
    Image out(500);
    EXPECT_EQ(fs.read(f, 8192 - 100, out), 100u);
    EXPECT_EQ(fs.read(f, 8192, out), 0u);
    EXPECT_EQ(fs.read(f, 100000, out), 0u);
}

TEST(MemcardFs, WriteClampsToFileSize) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 1);
    fs.create("G", 1);
    auto f = *fs.find("F");
    Image d(500, 0xAB);
    EXPECT_EQ(fs.write(f, 8192 - 100, d), 100u);
    EXPECT_EQ(fs.write(f, 8192, d), 0u);
    EXPECT_EQ(img[2 * 8192], 0);  // did not spill into the next file's block
}

TEST(MemcardFs, MultiBlockWriteSpansBlocks) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 3);
    auto f = *fs.find("F");
    Image d = pattern(3 * 8192, 5);
    EXPECT_EQ(fs.write(f, 0, d), d.size());
    Image out(d.size());
    EXPECT_EQ(fs.read(f, 0, out), d.size());
    EXPECT_EQ(out, d);
}

TEST(MemcardFs, CrossBlockAccessAtBoundary) {
    Image img = blank();
    CardFs fs(img);
    fs.create("F", 2);
    auto f = *fs.find("F");
    Image d = pattern(256, 11);
    fs.write(f, 8192 - 128, d);
    EXPECT_TRUE(std::equal(d.begin(), d.begin() + 128, img.begin() + 2 * 8192 - 128));
    EXPECT_TRUE(std::equal(d.begin() + 128, d.end(), img.begin() + 2 * 8192));
}

TEST(MemcardFs, FragmentedChainFollowsNextPointers) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.create("B", 1);
    fs.create("C", 1);
    fs.remove("A");
    fs.remove("C");
    ASSERT_EQ(fs.create("FRAG", 2), Status::Ok);  // gets blocks 1 and 3
    auto f = *fs.find("FRAG");
    EXPECT_EQ(f.blocks, (std::vector<uint8_t>{1, 3}));
    Image d = pattern(16384, 2);
    fs.write(f, 0, d);
    Image out(d.size());
    fs.read(f, 0, out);
    EXPECT_EQ(out, d);
    EXPECT_TRUE(std::equal(d.begin() + 8192, d.end(), img.begin() + 3 * 8192));
    EXPECT_EQ(fs.find("B")->blocks, std::vector<uint8_t>{2});
}

TEST(MemcardFs, WritesDoNotDisturbOtherFiles) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.create("B", 1);
    Image da = pattern(8192, 1), db = pattern(8192, 2);
    fs.write(*fs.find("A"), 0, da);
    fs.write(*fs.find("B"), 0, db);
    Image out(8192);
    fs.read(*fs.find("A"), 0, out);
    EXPECT_EQ(out, da);
    fs.read(*fs.find("B"), 0, out);
    EXPECT_EQ(out, db);
    EXPECT_TRUE(is_formatted(img));
}

TEST(MemcardFs, NewFileDataIsZeroEvenOverOldData) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.write(*fs.find("A"), 0, Image(8192, 0xCC));
    fs.remove("A");
    fs.create("B", 1);
    Image out(8192);
    fs.read(*fs.find("B"), 0, out);
    EXPECT_EQ(out, Image(8192, 0));
}

TEST(MemcardFs, RemoveMarksEntriesDeleted) {
    Image img = blank();
    CardFs fs(img);
    fs.create("CH", 3);
    ASSERT_EQ(fs.remove("CH"), Status::Ok);
    EXPECT_EQ(frame(img, 1)[0], 0xA1);
    EXPECT_EQ(frame(img, 2)[0], 0xA2);
    EXPECT_EQ(frame(img, 3)[0], 0xA3);
    for (std::size_t n = 1; n <= 3; ++n) EXPECT_TRUE(frame_checksum_ok(frame(img, n)));
    EXPECT_FALSE(fs.find("CH"));
    EXPECT_TRUE(fs.list().empty());
    EXPECT_EQ(fs.free_blocks(), 15);
}

TEST(MemcardFs, RemoveMissingFile) {
    Image img = blank();
    EXPECT_EQ(CardFs(img).remove("NOPE"), Status::NotFound);
}

TEST(MemcardFs, RemoveOnlyTargetsNamedFile) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.create("B", 2);
    fs.remove("A");
    EXPECT_TRUE(fs.find("B"));
    EXPECT_EQ(fs.free_blocks(), 13);
}

TEST(MemcardFs, DeletedBlocksAreReused) {
    Image img = blank();
    CardFs fs(img);
    for (int i = 0; i < 15; ++i) fs.create("F" + std::to_string(i), 1);
    fs.remove("F4");
    ASSERT_EQ(fs.create("NEW", 1), Status::Ok);
    EXPECT_EQ(fs.find("NEW")->blocks.front(), 5);
    EXPECT_EQ(fs.free_blocks(), 0);
}

TEST(MemcardFs, RecreateAfterDeleteWithSameName) {
    Image img = blank();
    CardFs fs(img);
    fs.create("S", 1);
    fs.remove("S");
    EXPECT_EQ(fs.create("S", 2), Status::Ok);
    EXPECT_EQ(fs.find("S")->blocks.size(), 2u);
}

TEST(MemcardFs, RenameChangesNameAndKeepsData) {
    Image img = blank();
    CardFs fs(img);
    fs.create("OLD", 1);
    Image d = pattern(128);
    fs.write(*fs.find("OLD"), 0, d);
    ASSERT_EQ(fs.rename("OLD", "NEWER_NAME"), Status::Ok);
    EXPECT_FALSE(fs.find("OLD"));
    auto f = fs.find("NEWER_NAME");
    ASSERT_TRUE(f);
    Image out(128);
    fs.read(*f, 0, out);
    EXPECT_EQ(out, d);
    EXPECT_TRUE(is_formatted(img));
}

TEST(MemcardFs, RenameToLongerThenShorterClearsOldBytes) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A_LONG_NAME_HERE", 1);
    fs.rename("A_LONG_NAME_HERE", "S");
    auto e = frame(img, 1);
    EXPECT_EQ(e[10], 'S');
    for (int i = 11; i < 30; ++i) EXPECT_EQ(e[i], 0) << i;
    EXPECT_EQ(fs.find("S")->name, "S");
}

TEST(MemcardFs, RenameErrors) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    fs.create("B", 1);
    EXPECT_EQ(fs.rename("MISSING", "C"), Status::NotFound);
    EXPECT_EQ(fs.rename("A", "B"), Status::Exists);
    EXPECT_EQ(fs.rename("A", ""), Status::BadArg);
    EXPECT_EQ(fs.rename("A", std::string(21, 'x')), Status::BadArg);
    EXPECT_TRUE(fs.find("A"));
}

TEST(MemcardFs, RenameToSameNameIsOk) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 1);
    EXPECT_EQ(fs.rename("A", "A"), Status::Ok);
}

TEST(MemcardFs, RenameMultiBlockFileKeepsChain) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 3);
    fs.rename("A", "B");
    EXPECT_EQ(fs.find("B")->blocks.size(), 3u);
}

TEST(MemcardFs, CorruptChainLoopTerminates) {
    Image img = blank();
    CardFs fs(img);
    fs.create("A", 2);
    img[2 * 128 + 8] = 0;  // block 2 points back to block 1
    seal_frame(std::span<uint8_t>(img.data() + 2 * 128, 128));
    auto f = fs.find("A");
    ASSERT_TRUE(f);
    EXPECT_LE(f->blocks.size(), 15u);
}

TEST(MemcardFs, StatusValuesAreBiosErrnos) {
    EXPECT_EQ(static_cast<int>(Status::NotFound), 2);
    EXPECT_EQ(static_cast<int>(Status::Exists), 0x11);
    EXPECT_EQ(static_cast<int>(Status::BadDevice), 0x13);
    EXPECT_EQ(static_cast<int>(Status::NoSpace), 0x1C);
}

// Every block count creates a correctly linked chain.
class ChainTest : public ::testing::TestWithParam<int> {};

TEST_P(ChainTest, CreateReadWriteRoundTrip) {
    int blocks = GetParam();
    Image img = blank();
    CardFs fs(img);
    ASSERT_EQ(fs.create("CHAIN", blocks), Status::Ok);
    auto f = *fs.find("CHAIN");
    ASSERT_EQ(f.blocks.size(), static_cast<std::size_t>(blocks));
    EXPECT_EQ(f.size, static_cast<uint32_t>(blocks) * 8192u);
    EXPECT_EQ(fs.free_blocks(), 15 - blocks);
    Image d = pattern(f.size, static_cast<uint8_t>(blocks));
    EXPECT_EQ(fs.write(f, 0, d), d.size());
    Image out(d.size());
    EXPECT_EQ(fs.read(f, 0, out), d.size());
    EXPECT_EQ(out, d);
    EXPECT_TRUE(is_formatted(img));
    ASSERT_EQ(fs.remove("CHAIN"), Status::Ok);
    EXPECT_EQ(fs.free_blocks(), 15);
}

INSTANTIATE_TEST_SUITE_P(Blocks, ChainTest, ::testing::Range(1, 16));

class FrameAccessTest : public ::testing::TestWithParam<int> {};

TEST_P(FrameAccessTest, EachFrameOfAFileIsAddressable) {
    int fr = GetParam();  // frame index within a 2-block file
    Image img = blank();
    CardFs fs(img);
    fs.create("X", 2);
    auto f = *fs.find("X");
    Image d = pattern(128, static_cast<uint8_t>(fr));
    EXPECT_EQ(fs.write(f, static_cast<std::size_t>(fr) * 128, d), 128u);
    std::size_t blk = static_cast<std::size_t>(f.blocks[static_cast<std::size_t>(fr) / 64]);
    std::size_t at = blk * 8192 + (static_cast<std::size_t>(fr) % 64) * 128;
    EXPECT_TRUE(std::equal(d.begin(), d.end(), img.begin() + static_cast<std::ptrdiff_t>(at)));
}

INSTANTIATE_TEST_SUITE_P(Frames, FrameAccessTest, ::testing::Values(0, 1, 31, 62, 63, 64, 65, 100, 126, 127));

// ---- .mcd files -----------------------------------------------------------------------------

TEST(MemcardFile, SaveLoadRoundTrip) {
    auto p = temp_path("roundtrip.mcd");
    Image img = blank();
    CardFs fs(img);
    fs.create("PERSIST", 2);
    fs.write(*fs.find("PERSIST"), 0, pattern(16384, 4));
    ASSERT_TRUE(save_card_file(p, img));
    EXPECT_EQ(std::filesystem::file_size(p), kCardSize);
    auto loaded = load_card_file(p);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(*loaded, img);
    std::filesystem::remove(p);
}

TEST(MemcardFile, SaveLeavesNoTempFile) {
    auto p = temp_path("notmp.mcd");
    ASSERT_TRUE(save_card_file(p, blank()));
    EXPECT_FALSE(std::filesystem::exists(p.string() + ".tmp"));
    std::filesystem::remove(p);
}

TEST(MemcardFile, SaveOverwritesExisting) {
    auto p = temp_path("overwrite.mcd");
    save_card_file(p, Image(kCardSize, 1));
    save_card_file(p, Image(kCardSize, 2));
    EXPECT_EQ((*load_card_file(p))[0], 2);
    std::filesystem::remove(p);
}

TEST(MemcardFile, SaveToMissingDirectoryFails) {
    EXPECT_FALSE(save_card_file(std::filesystem::temp_directory_path() / "dw3_no_such_dir" / "x.mcd", blank()));
}

TEST(MemcardFile, LoadMissingFile) { EXPECT_FALSE(load_card_file(temp_path("missing.mcd"))); }

TEST(MemcardFile, LoadRejectsWrongSizes) {
    auto p = temp_path("size.mcd");
    for (std::size_t sz : {std::size_t{0}, std::size_t{1000}, kCardSize - 1, kCardSize + 1, kCardSize * 2}) {
        std::ofstream(p, std::ios::binary).write(std::string(sz, 'x').data(), static_cast<std::streamsize>(sz));
        EXPECT_FALSE(load_card_file(p)) << sz;
    }
    std::filesystem::remove(p);
}

TEST(MemcardFile, LoadAcceptsGmeHeader) {
    auto p = temp_path("card.gme");
    Image img = blank();
    Image file(3904, 0);
    std::copy_n("123-456-STD", 11, file.begin());
    file.insert(file.end(), img.begin(), img.end());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                              static_cast<std::streamsize>(file.size()));
    auto l = load_card_file(p);
    ASSERT_TRUE(l);
    EXPECT_EQ(*l, img);
    std::filesystem::remove(p);
}

TEST(MemcardFile, LoadAcceptsVgsHeader) {
    auto p = temp_path("card.vgs");
    Image img = blank();
    Image file(64, 0);
    std::copy_n("VgsM", 4, file.begin());
    file.insert(file.end(), img.begin(), img.end());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                              static_cast<std::streamsize>(file.size()));
    auto l = load_card_file(p);
    ASSERT_TRUE(l);
    EXPECT_EQ(*l, img);
    std::filesystem::remove(p);
}

TEST(MemcardFile, GmeSizeWithoutSignatureIsRejected) {
    auto p = temp_path("fake.gme");
    Image file(kCardSize + 3904, 0);
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                              static_cast<std::streamsize>(file.size()));
    EXPECT_FALSE(load_card_file(p));
    std::filesystem::remove(p);
}

TEST(MemcardFile, LoadedCardRemainsUsable) {
    auto p = temp_path("usable.mcd");
    save_card_file(p, blank());
    auto l = *load_card_file(p);
    CardFs fs(l);
    EXPECT_TRUE(fs.valid());
    EXPECT_EQ(fs.create("X", 1), Status::Ok);
    std::filesystem::remove(p);
}

}  // namespace
