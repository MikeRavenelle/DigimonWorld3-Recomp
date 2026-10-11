// Scenario file format, input scripting and perceptual hash (no game needed).

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "scenario.h"

using namespace dw3::scenario;

namespace {
constexpr uint16_t kStart = 1u << 3, kUp = 1u << 4, kCross = 1u << 14;
}

TEST(ScenarioFormat, MinimalFileDefaults) {
    Scenario s = parse("name = \"t\"\nframes = 100\n");
    EXPECT_EQ(s.name, "t");
    EXPECT_EQ(s.frames, 100);
    EXPECT_TRUE(s.skip_intro);
    EXPECT_EQ(s.memcard, "none");
    EXPECT_TRUE(s.presses.empty());
    EXPECT_TRUE(s.checks.empty());
}

TEST(ScenarioFormat, MissingNameOrFramesRejected) {
    EXPECT_THROW(parse("frames = 10\n"), std::runtime_error);
    EXPECT_THROW(parse("name = \"x\"\n"), std::runtime_error);
    EXPECT_THROW(parse("name = \"x\"\nframes = 0\n"), std::runtime_error);
}

TEST(ScenarioFormat, BadMemcardRejected) {
    EXPECT_THROW(parse("name = \"x\"\nframes = 1\nmemcard = \"floppy\"\n"), std::runtime_error);
}

TEST(ScenarioFormat, TomlSyntaxErrorReported) {
    try {
        parse("name = \n", "bad.toml");
        FAIL();
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("bad.toml"), std::string::npos);
    }
}

TEST(ScenarioFormat, PressDefaultsHoldFour) {
    Scenario s = parse("name=\"t\"\nframes=100\nskip_intro=false\n[[press]]\nbutton=\"UP\"\nat=10\n");
    ASSERT_EQ(s.presses.size(), 1u);
    EXPECT_EQ(s.presses[0].button, kUp);
    EXPECT_EQ(s.presses[0].from, 10);
    EXPECT_EQ(s.presses[0].to, 14);
}

TEST(ScenarioFormat, UnknownButtonRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=100\n[[press]]\nbutton=\"JUMP\"\nat=1\n"), std::runtime_error);
}

TEST(ScenarioFormat, RepeatExpands) {
    Scenario s =
        parse("name=\"t\"\nframes=100\n[[repeat]]\nbutton=\"CROSS\"\nfrom=10\nto=50\nevery=10\nhold=2\n");
    ASSERT_EQ(s.presses.size(), 4u);
    EXPECT_EQ(s.presses[3].from, 40);
    EXPECT_EQ(s.presses[3].to, 42);
}

TEST(ScenarioFormat, RepeatNeedsEveryGreaterThanHold) {
    EXPECT_THROW(parse("name=\"t\"\nframes=100\n[[repeat]]\nbutton=\"CROSS\"\nevery=3\nhold=3\n"), std::runtime_error);
}

TEST(ScenarioFormat, ChecksParsedAndSorted) {
    Scenario s = parse(R"(name="t"
frames=500
[[check]]
at=400
label="late"
min_primitives=10
[[check]]
at=100
display_enabled=true
display_width=320
display_height=240
tty_contains="CD_init"
screenshot=true
ram=[{addr=0x800A0000, size=2, value=7}]
)");
    ASSERT_EQ(s.checks.size(), 2u);
    EXPECT_EQ(s.checks[0].at, 100);
    EXPECT_EQ(*s.checks[0].display_enabled, true);
    EXPECT_EQ(*s.checks[0].display_width, 320);
    EXPECT_EQ(*s.checks[0].tty_contains, "CD_init");
    EXPECT_TRUE(s.checks[0].screenshot);
    ASSERT_EQ(s.checks[0].ram.size(), 1u);
    EXPECT_EQ(s.checks[0].ram[0].addr, 0x800A0000u);
    EXPECT_EQ(s.checks[0].ram[0].size, 2);
    EXPECT_EQ(s.checks[1].label, "late");
    EXPECT_EQ(*s.checks[1].min_primitives, 10u);
}

TEST(ScenarioFormat, CheckOutsideRangeRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=10\n[[check]]\nat=11\n"), std::runtime_error);
}

TEST(ScenarioFormat, BadRamSizeRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=10\n[[check]]\nat=1\nram=[{addr=0, size=3, value=0}]\n"),
                 std::runtime_error);
}

TEST(ScenarioFormat, BadHashRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=10\n[[check]]\nat=1\nscreen_hash=\"abc\"\n"), std::runtime_error);
}

TEST(ScenarioInput, IntroSkipPressesStartAt300) {
    Scenario s = parse("name=\"t\"\nframes=1000\n");
    EXPECT_EQ(buttons_at(s, 299), 0);
    EXPECT_EQ(buttons_at(s, 300), kStart);
    EXPECT_EQ(buttons_at(s, 303), kStart);
    EXPECT_EQ(buttons_at(s, 304), 0);
}

TEST(ScenarioInput, IntroSkipCanBeDisabled) {
    Scenario s = parse("name=\"t\"\nframes=1000\nskip_intro=false\n");
    EXPECT_EQ(buttons_at(s, 300), 0);
}

TEST(ScenarioInput, OverlappingPressesCombine) {
    Scenario s = parse(
        "name=\"t\"\nframes=100\nskip_intro=false\n[[press]]\nbutton=\"UP\"\nat=10\nhold=10\n"
        "[[press]]\nbutton=\"CROSS\"\nat=15\n");
    EXPECT_EQ(buttons_at(s, 12), kUp);
    EXPECT_EQ(buttons_at(s, 16), kUp | kCross);
    EXPECT_EQ(buttons_at(s, 20), 0);
}

TEST(ScenarioInput, ButtonNamesMatchPadBits) {
    EXPECT_EQ(button_from_name("SELECT"), 1u << 0);
    EXPECT_EQ(button_from_name("START"), 1u << 3);
    EXPECT_EQ(button_from_name("LEFT"), 1u << 7);
    EXPECT_EQ(button_from_name("SQUARE"), 1u << 15);
    EXPECT_EQ(button_from_name("start"), 0u);  // names are upper case
}

TEST(ScenarioHash, HexRoundTrip) {
    std::array<uint64_t, 4> h{0x0123456789ABCDEFull, 0, ~0ull, 0x8000000000000001ull};
    auto back = hash_from_hex(hash_to_hex(h));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, h);
    EXPECT_EQ(hash_to_hex(h).size(), 64u);
    EXPECT_FALSE(hash_from_hex(std::string(64, 'g')));
}

TEST(ScenarioHash, IdenticalImagesDistanceZero) {
    std::vector<uint32_t> img(320 * 240);
    for (std::size_t i = 0; i < img.size(); ++i) img[i] = 0xFF000000u | static_cast<uint32_t>(i * 2654435761u);
    auto a = perceptual_hash(img.data(), 320, 240);
    EXPECT_EQ(hash_distance(a, perceptual_hash(img.data(), 320, 240)), 0);
}

TEST(ScenarioHash, LeftRightSplitHashesHalfBits) {
    std::vector<uint32_t> img(256 * 256);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) img[static_cast<std::size_t>(y * 256 + x)] = x >= 128 ? 0xFFFFFFFFu : 0xFF000000u;
    auto h = perceptual_hash(img.data(), 256, 256);
    int bits = 0;
    for (uint64_t w : h) bits += std::popcount(w);
    EXPECT_EQ(bits, 128);
}

TEST(ScenarioHash, SmallNoiseStaysClose) {
    std::vector<uint32_t> a(320 * 240), b;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            uint32_t v = static_cast<uint32_t>((x * 255) / 319);
            a[static_cast<std::size_t>(y * 320 + x)] = 0xFF000000u | v | (v << 8) | (v << 16);
        }
    b = a;
    for (std::size_t i = 0; i < b.size(); i += 97) b[i] ^= 0x00030303u;  // sparse tiny changes
    EXPECT_LE(hash_distance(perceptual_hash(a.data(), 320, 240), perceptual_hash(b.data(), 320, 240)), 4);
}

TEST(ScenarioHash, DifferentImagesFarApart) {
    std::vector<uint32_t> a(320 * 240), b(320 * 240);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            a[static_cast<std::size_t>(y * 320 + x)] = x < 160 ? 0xFFFFFFFFu : 0xFF000000u;
            b[static_cast<std::size_t>(y * 320 + x)] = y < 120 ? 0xFFFFFFFFu : 0xFF000000u;
        }
    EXPECT_GE(hash_distance(perceptual_hash(a.data(), 320, 240), perceptual_hash(b.data(), 320, 240)), 100);
}

// ---- Audio checks ----

TEST(ScenarioAudio, CheckFieldsParsed) {
    Scenario s = parse(R"(name="t"
frames=100
[[check]]
at=50
audio_min_rms=100.5
audio_max_rms=9000
audio_frames=30
wav=true
)");
    ASSERT_EQ(s.checks.size(), 1u);
    EXPECT_DOUBLE_EQ(*s.checks[0].audio_min_rms, 100.5);
    EXPECT_DOUBLE_EQ(*s.checks[0].audio_max_rms, 9000.0);
    EXPECT_EQ(s.checks[0].audio_frames, 30);
    EXPECT_TRUE(s.checks[0].wav);
}

TEST(ScenarioAudio, Defaults) {
    Scenario s = parse("name=\"t\"\nframes=100\n[[check]]\nat=50\n");
    EXPECT_FALSE(s.checks[0].audio_min_rms);
    EXPECT_FALSE(s.checks[0].audio_max_rms);
    EXPECT_EQ(s.checks[0].audio_frames, 60);
    EXPECT_FALSE(s.checks[0].wav);
}

TEST(ScenarioAudio, ZeroWindowRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=100\n[[check]]\nat=50\naudio_frames=0\n"), std::runtime_error);
}

TEST(ScenarioAudio, MinAboveMaxRejected) {
    EXPECT_THROW(parse("name=\"t\"\nframes=100\n[[check]]\nat=50\naudio_min_rms=10.0\naudio_max_rms=5.0\n"),
                 std::runtime_error);
}

TEST(ScenarioAudio, MeterOfSilenceIsZero) {
    AudioMeter m;
    std::vector<int16_t> z(200, 0);
    m.add_frame(z);
    EXPECT_EQ(m.rms(60), 0.0);
    EXPECT_EQ(m.frames(), 1u);
}

TEST(ScenarioAudio, MeterOfEmptyIsZero) {
    AudioMeter m;
    EXPECT_EQ(m.rms(10), 0.0);
    m.add_frame({});
    EXPECT_EQ(m.rms(10), 0.0);
}

TEST(ScenarioAudio, MeterConstantSignalRms) {
    AudioMeter m;
    std::vector<int16_t> c(100, 1000);
    for (int i = 0; i < 5; ++i) m.add_frame(c);
    EXPECT_DOUBLE_EQ(m.rms(5), 1000.0);
    EXPECT_DOUBLE_EQ(m.rms(1), 1000.0);
}

TEST(ScenarioAudio, MeterSquareWaveRms) {
    AudioMeter m;
    std::vector<int16_t> sq;
    for (int i = 0; i < 100; ++i) sq.push_back(i % 2 ? 3000 : -3000);
    m.add_frame(sq);
    EXPECT_DOUBLE_EQ(m.rms(1), 3000.0);
}

TEST(ScenarioAudio, MeterWindowCoversOnlyLastFrames) {
    AudioMeter m;
    std::vector<int16_t> loud(100, 8000), quiet(100, 0);
    m.add_frame(loud);
    for (int i = 0; i < 10; ++i) m.add_frame(quiet);
    EXPECT_EQ(m.rms(10), 0.0);
    EXPECT_GT(m.rms(11), 0.0);
    EXPECT_NEAR(m.rms(11), 8000.0 / std::sqrt(11.0), 1e-6);
}

TEST(ScenarioAudio, MeterWindowLargerThanHistoryUsesAll) {
    AudioMeter m;
    std::vector<int16_t> c(10, 500);
    m.add_frame(c);
    EXPECT_DOUBLE_EQ(m.rms(1000), 500.0);
}

TEST(ScenarioAudio, MeterWeightsFramesBySampleCount) {
    AudioMeter m;
    m.add_frame(std::vector<int16_t>(300, 1000));
    m.add_frame(std::vector<int16_t>(100, 0));
    EXPECT_NEAR(m.rms(2), std::sqrt(300.0 * 1e6 / 400.0), 1e-6);
}

TEST(ScenarioAudio, MeterKeepsSamplesOnlyWhenAsked) {
    AudioMeter off(false), on(true);
    std::vector<int16_t> c = {1, 2, 3, 4};
    off.add_frame(c);
    on.add_frame(c);
    EXPECT_TRUE(off.all().empty());
    EXPECT_EQ(on.all(), c);
    EXPECT_TRUE(off.window(1).empty());
}

TEST(ScenarioAudio, MeterWindowSamples) {
    AudioMeter m(true);
    m.add_frame(std::vector<int16_t>{1, 1});
    m.add_frame(std::vector<int16_t>{2, 2});
    m.add_frame(std::vector<int16_t>{3, 3});
    EXPECT_EQ(m.window(2), (std::vector<int16_t>{2, 2, 3, 3}));
    EXPECT_EQ(m.window(10), (std::vector<int16_t>{1, 1, 2, 2, 3, 3}));
}

TEST(ScenarioAudio, WavHeaderLayout) {
    std::vector<int16_t> pcm = {1, -1, 256, -256};
    auto b = wav_bytes(pcm);
    ASSERT_EQ(b.size(), 44u + 8u);
    EXPECT_EQ(std::string(b.begin(), b.begin() + 4), "RIFF");
    EXPECT_EQ(std::string(b.begin() + 8, b.begin() + 12), "WAVE");
    EXPECT_EQ(std::string(b.begin() + 12, b.begin() + 16), "fmt ");
    EXPECT_EQ(std::string(b.begin() + 36, b.begin() + 40), "data");
    auto u32 = [&](std::size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24); };
    EXPECT_EQ(u32(4), 36u + 8u);
    EXPECT_EQ(u32(16), 16u);
    EXPECT_EQ(b[20], 1);   // PCM
    EXPECT_EQ(b[22], 2);   // stereo
    EXPECT_EQ(u32(24), 44100u);
    EXPECT_EQ(u32(28), 44100u * 4);
    EXPECT_EQ(b[32], 4);   // block align
    EXPECT_EQ(b[34], 16);  // bits
    EXPECT_EQ(u32(40), 8u);
}

TEST(ScenarioAudio, WavSamplesLittleEndian) {
    auto b = wav_bytes(std::vector<int16_t>{0x1234, -2});
    EXPECT_EQ(b[44], 0x34);
    EXPECT_EQ(b[45], 0x12);
    EXPECT_EQ(b[46], 0xFE);
    EXPECT_EQ(b[47], 0xFF);
}

TEST(ScenarioAudio, WavCustomRate) {
    auto b = wav_bytes(std::vector<int16_t>{0, 0}, 22050);
    EXPECT_EQ(b[24] | (b[25] << 8), 22050);
}

TEST(ScenarioAudio, WavFileWritten) {
    auto path = std::filesystem::temp_directory_path() / "dw3_scenario_test" / "x.wav";
    ASSERT_TRUE(write_wav(path.string(), std::vector<int16_t>{1, 2, 3, 4}));
    EXPECT_EQ(std::filesystem::file_size(path), 44u + 8u);
    std::filesystem::remove_all(path.parent_path());
}

TEST(ScenarioAudio, WavUnwritablePathFails) {
    EXPECT_FALSE(write_wav("/proc/definitely/not/here.wav", std::vector<int16_t>{1, 2}));
}
