#pragma once

// In-game scenario tests: scripted input + timed checkpoints, loaded from TOML files in tests/scenarios/.
//
//   name = "newgame"
//   frames = 3000
//   skip_intro = true          # default; presses START at frame 300 to skip the intro FMV
//   memcard = "none"           # "none" | "blank" (fresh formatted in-memory card)
//
//   [[reboot]]                 # power-cycle the machine at frame `at`: RAM, GPU and game state are lost, the
//   at = 4000                  # memory card image is kept. Frames in the file stay global; the intro skip
//                              # (when enabled) is pressed kIntroSkipFrame frames after every boot.
//
//   [[press]]                  # hold `button` from `at` for `hold` frames (default 4)
//   button = "START"
//   at = 900
//
//   [[repeat]]                 # press `button` every `every` frames in [from, to)
//   button = "CROSS"
//   from = 1100
//   to = 3000
//   every = 45
//
//   [[wait]]                   # RAM-driven pause of the script clock (see below)
//   at = 2000                  # script frame at which the wait starts
//   label = "battle started"
//   ram = [{ addr = 0x8007A000, size = 1, value = 1 }]   # all conditions must hold (same fields as in [[check]])
//   timeout = 1800             # real frames; the scenario fails when the condition is still false
//   hold = ["UP", "RIGHT"]     # optional: buttons held for the whole wait
//   press = "CROSS"            # optional: button tapped every `every` frames (default 40) for `press_hold` (default 4)
//
//   Script time versus real frames: `at`/`from`/`to`/`frames` of presses, repeats, checks and reboots are in
//   script frames. The script clock normally equals the real frame count, but it stands still while a [[wait]]
//   is pending and resumes when its condition holds (or fails the run on timeout). Buttons held at the wait's
//   script frame by a [[press]] stay held during the wait. Checks at the wait's own frame run before it ends.
//
//   [[check]]                  # all fields optional except `at`
//   at = 2000
//   label = "field scene"
//   display_enabled = true
//   display_width = 320
//   display_height = 240
//   min_primitives = 1000      # cumulative GPU primitives since boot
//   min_commands = 1000        # cumulative GPU commands since boot
//   tty_contains = "CD_init"   # game debug output seen so far
//   screen_hash = "0123..."    # 256-bit perceptual hash of the displayed image (64 hex chars)
//   max_hash_distance = 24     # allowed Hamming distance (default 24)
//   ram = [{ addr = 0x800A0000, size = 4, value = 1 }]   # optional `mask` (AND before comparing, default all
//                              # bits) and `cmp` = "eq" (default) | "ne" | "lt" | "le" | "gt" | "ge"
//   screenshot = true          # save BMP of the display (for review; never committed)
//   audio_min_rms = 100.0      # RMS (int16 units, both channels) of the mixed output over the last `audio_frames` video frames
//   audio_max_rms = 20000.0
//   audio_frames = 60          # window length in video frames (default 60)
//   wav = true                 # save that audio window as <out>/<name>/<at>_<label>.wav (never committed)

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dw3::scenario {

struct Press {
    uint16_t button = 0;
    int64_t from = 0, to = 0;  // frames, [from, to)
};

enum class Compare { Eq, Ne, Lt, Le, Gt, Ge };

struct RamCheck {
    uint32_t addr = 0;
    int size = 4;
    uint32_t value = 0;
    uint32_t mask = 0xFFFFFFFFu;
    Compare cmp = Compare::Eq;
};

// True when `(actual & mask) <cmp> (value & mask)`.
bool ram_matches(const RamCheck& r, uint32_t actual);

// Pauses the script clock at script frame `at` until every `ram` condition holds.
struct Wait {
    int64_t at = 0;
    std::string label;
    std::vector<RamCheck> ram;
    int64_t timeout = 0;  // real frames
    uint16_t hold = 0;  // buttons held during the wait
    uint16_t tap = 0;  // button tapped during the wait (0 = none)
    int64_t tap_every = 40, tap_hold = 4;
};

struct Check {
    int64_t at = 0;
    std::string label;
    std::optional<bool> display_enabled;
    std::optional<int> display_width, display_height;
    std::optional<uint64_t> min_primitives, min_commands;
    std::optional<std::string> tty_contains;
    std::optional<std::array<uint64_t, 4>> screen_hash;
    int max_hash_distance = 24;
    std::vector<RamCheck> ram;
    bool screenshot = false;
    std::optional<double> audio_min_rms, audio_max_rms;
    int audio_frames = 60;
    bool wav = false;
};

struct Scenario {
    std::string name;
    std::string description;
    int64_t frames = 0;
    bool skip_intro = true;
    std::string memcard = "none";
    std::vector<Press> presses;
    std::vector<int64_t> reboots;  // sorted, within (0, frames)
    std::vector<Wait> waits;  // sorted by `at`
    std::vector<Check> checks;  // sorted by `at`
};

constexpr int64_t kIntroSkipFrame = 300;

// Parses a scenario file. Throws std::runtime_error with a readable message on errors.
Scenario load(const std::string& path);
Scenario parse(const std::string& toml_text, const std::string& source_name = "<string>");

uint16_t button_from_name(const std::string& name);  // 0 if unknown

// Collects the mixed audio of each video frame and answers windowed RMS queries.
class AudioMeter {
public:
    explicit AudioMeter(bool keep_samples = false) : keep_(keep_samples) {}
    // Adds one video frame's worth of interleaved stereo int16 samples (may be empty).
    void add_frame(std::span<const int16_t> interleaved);
    std::size_t frames() const { return blocks_.size(); }  // video frames added
    // RMS over the last n video frames (all if fewer); 0 if no samples.
    double rms(int n) const;
    // Samples of the last n video frames (needs keep_samples).
    std::vector<int16_t> window(int n) const;
    const std::vector<int16_t>& all() const { return samples_; }

private:
    struct Block {
        double sum_sq = 0;
        std::size_t count = 0, first = 0;  // samples (not frames); first = offset into samples_
    };
    bool keep_;
    std::vector<Block> blocks_;
    std::vector<int16_t> samples_;
};

// 44-byte-header PCM WAV (16-bit stereo) bytes / file writer.
std::vector<uint8_t> wav_bytes(std::span<const int16_t> interleaved_stereo, uint32_t sample_rate = 44100);
bool write_wav(const std::string& path, std::span<const int16_t> interleaved_stereo, uint32_t sample_rate = 44100);

// Global frame at which the boot running at `frame` started (0 before the first reboot).
int64_t boot_start(const Scenario& s, int64_t frame);

// Buttons held at `frame` (including the automatic intro skip after the boot that is running).
uint16_t buttons_at(const Scenario& s, int64_t frame);

// Maps real frames to script frames and decides the pad state, honouring [[wait]] entries. Call advance() once
// per real frame (after the frame ran) with a RAM reader; it then reports the script frame and the buttons for the
// next frame. Without waits the script frame equals the real frame.
class Timeline {
public:
    using RamReader = std::function<uint32_t(uint32_t addr, int size)>;
    explicit Timeline(const Scenario& s) : s_(s) {}

    void advance(int64_t real_frame, const RamReader& read);
    int64_t script_frame() const { return script_; }
    uint16_t buttons() const;
    bool waiting() const { return waiting_; }
    // Set once a wait ran out of time; `failed_wait()` is that wait.
    bool timed_out() const { return timed_out_; }
    const Wait* failed_wait() const { return timed_out_ ? &s_.waits[next_] : nullptr; }
    // Real frames the most recently finished wait took (0 before the first).
    int64_t last_wait_frames() const { return last_wait_frames_; }
    std::size_t completed_waits() const { return next_; }

private:
    bool satisfied(const Wait& w, const RamReader& read) const;
    const Scenario& s_;
    std::size_t next_ = 0;  // index of the pending or active wait
    bool waiting_ = false, timed_out_ = false;
    int64_t offset_ = 0, script_ = 0, real_ = 0, wait_start_ = 0, last_wait_frames_ = 0;
};

// 256-bit perceptual hash: image box-downscaled to 16x16 luma, each bit = cell brighter than mean.
std::array<uint64_t, 4> perceptual_hash(const uint32_t* rgba, int width, int height);
int hash_distance(const std::array<uint64_t, 4>& a, const std::array<uint64_t, 4>& b);
std::string hash_to_hex(const std::array<uint64_t, 4>& h);
std::optional<std::array<uint64_t, 4>> hash_from_hex(const std::string& hex);

}  // namespace dw3::scenario
