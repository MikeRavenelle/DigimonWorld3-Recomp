#pragma once

// In-game scenario tests: scripted input + timed checkpoints, loaded from TOML files in tests/scenarios/.
//
//   name = "newgame"
//   frames = 3000
//   skip_intro = true          # default; presses START at frame 300 to skip the intro FMV
//   memcard = "none"           # "none" | "blank" (fresh formatted in-memory card) | "game:<file>" (a raw 128 KiB card
//                              # image under the game directory, e.g. "game:memcards/endgame.mcd"; it is copied into
//                              # an in-memory card, so the file is never written; a zero
//                              # checksum in the header frame, as DexDrive conversions leave, is repaired. A missing file
//                              # skips the scenario.)
//
//   settings = ["gameplay.encounters=off"]   # application settings for this run, as id=value (ids as in settings.toml);
//                              # everything else keeps its default
//   gpu = true                 # draw with a headless GPU renderer instead of the software rasterizer (skips when the
//                              # machine has no usable GPU); implied by a graphics.widescreen setting
//
//   [[reboot]]                 # power-cycle the machine at frame `at`: RAM, GPU and game state are lost, the
//   at = 4000                  # memory card image is kept. Frames in the file stay global; the intro skip
//                              # (when enabled) is pressed kIntroSkipFrame frames after every boot.
//
//   [[savestate]]              # write a save state once the script reaches frame `at` (a relative path is placed
//   at = 1472                  # under --state-dir). A state can be taken only at a safe point of the recompiled
//   path = "new_game.state"    # game, so it is written within a few emulated instructions after that frame.
//
//   [[loadstate]]              # restore a save state at frame `at`; the script clock and real frame counter keep
//   at = 1500                  # running from their current values.
//   path = "new_game.state"
//   replay = false             # true: also rewind the script to the matching [[savestate]] and re-run it, requiring
//                              # every frame in between to match the first pass (RAM, VRAM and display hashes)
//
//   start_state = "new_game.state"   # top level: boot, then load this state before the first frame (the intro skip
//   start_frame = 1472               # should be disabled). The script clock starts at `start_frame`, so entries keep
//                                    # the frame numbers of the scenario that produced the state.
//
//   [[press]]                  # hold `button` from `at` for `hold` frames (default 4)
//   button = "START"
//   at = 900
//
//   [[poke]]                   # write `value` (little endian, `size` 1, 2 or 4 bytes) to RAM `addr` at script frame `at`,
//   at = 5135                  # for states the game's own play does not reach cheaply (e.g. training points)
//   addr = 0x80048C6E
//   size = 2
//   value = 50
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
//   [[walk]]                   # steering wait: holds d-pad directions toward a target position, see below
//   at = 20000
//   label = "to the gate"
//   x = { via = 0x8004ADC0, addr = 0x58, size = 4 }   # position words (signed); `via` = pointer to an object
//   z = { via = 0x8004ADC0, addr = 0x5C, size = 4 }   # and `addr` the offset into it (see [[check]] ram)
//   target = [1000, 480]       # x and z to reach
//   tolerance = 12             # |dx| and |dz| must both end up within this
//   axes = "xz"                # default; "x" or "z" steers and tests one axis only (corridors, where the player
//                              # must not press the other direction)
//   timeout = 900              # real frames
//   guard = [{ addr = 0x800554D4, size = 4, value = 0x1C3 }]   # must hold during the walk, else the run fails
//   ram = [...]                # optional extra conditions that must also hold at the end
//   press = "CROSS"            # optional tap, like [[wait]]
//   While a walk is pending, LEFT/RIGHT are held while x is more than `tolerance` away from the target in that
//   direction (RIGHT when x is too small) and UP/DOWN likewise for z (DOWN when z is too small). Wall collisions are
//   not modelled: a route around obstacles is a sequence of walks to waypoints.
//
//   include = ["common/new_game.toml"]   # entries ([[press]], [[repeat]], [[wait]], [[walk]], [[check]], [[reboot]], [[poke]])
//                              # of the listed files (paths relative to this file) are merged in before this file's own;
//                              # their other keys are ignored. Frames in all files share one script clock.
//
//   RAM addresses found so far (details in tests/scenarios/common/new_game.toml):
//     0x800554D4 map id, 0x800554D0 game mode (2 field, 6 battle, 0x14 results, 0xC save/load screen),
//     0x8004ADC0 pointer to the player object (+0x58 x, +0x5C z, +0x60 facing 0..7), 0x80099114 event busy flag,
//     0x800A3260 enemy HP (half-word).
//
//   Script time versus real frames: `at`/`from`/`to`/`frames` of presses, repeats, checks and reboots are in
//   script frames. The script clock normally equals the real frame count, but it stands still while a [[wait]]
//   is pending and resumes when its condition holds (or fails the run on timeout). Buttons held at the wait's
//   script frame by a [[press]] stay held during the wait. Checks at the wait's own frame run before it ends.
//
//   [[flicker_watch]]          # needs `gpu = true`; see FlickerWatch below
//   from = 3905
//   to = 3990
//   label = "battle menu"
//   flicker_blocks_max = 40
//
//   [[margin_watch]]           # needs `gpu = true` (or a widescreen setting); see MarginWatch below
//   from = 1486
//   to = 1486
//   label = "map load"
//   lit_changes_max = 3
//   strip = true
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
//   disc_backgrounds = 8       # at least this many (image, palette) keys of field map backgrounds drawn since boot are
//                              # also computed from the stage packs on the disc, and none of the images is drawn with
//                              # a palette the file does not give it (the renderer's keys against the game files')
//   screen_hash = "0123..."    # 256-bit perceptual hash of the displayed image (64 hex chars)
//   max_hash_distance = 24     # allowed Hamming distance (default 24)
//   ram = [{ addr = 0x800A0000, size = 4, value = 1 }]   # optional `mask` (AND before comparing, default all
//                              # bits) and `cmp` = "eq" (default) | "ne" | "lt" | "le" | "gt" | "ge"; with `via` = P
//                              # the address is the word at P plus `addr` (false while P is not a RAM pointer)
//   screenshot = true          # save BMP of the display (for review; never committed)
//   margin_colors_min = 8      # needs `gpu = true` (or a widescreen setting): the picture rendered by the GPU renderer at
//   margin_colors_max = 1      # 1280x720 must show at least / at most that many distinct colours in the outer 10% of its
//                              # columns (1 is the black pillarbox of a 4:3 picture)
//   backdrop_pieces_min = 1   # needs `gpu = true`: totals since the start of the run for the seamless field background:
//   squares_hidden_min = 20   # pieces the renderer drew that the game had not, the "not loaded yet" squares it hid, and
//   squares_drawn_min = 20    # the ones it let through (the game draws an opaque 64x64 square, 4-bit texture, over every
//   squares_drawn_max = 0     # cell that is not loaded; with `graphics.seamless_bg` they are hidden instead)
//   wide_field_frames_min = 100       # needs a widescreen setting: totals since the start of the run for wide fields: finished
//   wide_field_fallbacks_max = 200    # frames shown wide / shown at 4:3 because the map was not resolved, and textured
//   wide_margin_polygons_min = 1      # polygons (characters, objects) the game drew that reached the margins (_max: at most)
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
    uint32_t via = 0;  // non-zero: address of a pointer word; `addr` is then an offset from that pointer
    uint32_t addr = 0;
    int size = 4;
    uint32_t value = 0;
    uint32_t mask = 0xFFFFFFFFu;
    Compare cmp = Compare::Eq;
};

// True when `(actual & mask) <cmp> (value & mask)`.
bool ram_matches(const RamCheck& r, uint32_t actual);

using RamReader = std::function<uint32_t(uint32_t addr, int size)>;

// Reads the location a RamCheck names (following `via`). False when the pointer does not point into RAM.
bool read_ram(const RamReader& read, const RamCheck& r, uint32_t& out);
// Sign-extends the low `size` bytes of `v`.
int32_t sign_extend(uint32_t v, int size);

// Target position of a [[walk]]: x/z are read through RamCheck (only the address fields matter).
struct Steer {
    RamCheck x, z;
    int32_t target_x = 0, target_z = 0, tolerance = 8;
    bool use_x = true, use_z = true;  // axes that take part (a corridor leg steers one axis only)
};

// d-pad buttons that move (x, z) toward the target: LEFT/RIGHT for x, UP/DOWN for z, none for an axis within
// tolerance or left out of the walk.
uint16_t steer_buttons(const Steer& s, int32_t x, int32_t z);
bool steer_reached(const Steer& s, int32_t x, int32_t z);

// Pauses the script clock at script frame `at` until every `ram` condition holds.
struct Wait {
    int64_t at = 0;
    std::string label;
    std::vector<RamCheck> ram;
    std::optional<Steer> steer;  // set for [[walk]]
    std::vector<RamCheck> guard;  // must hold while the wait is pending (walks)
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
    std::optional<int> min_disc_backgrounds;
    std::optional<std::array<uint64_t, 4>> screen_hash;
    std::optional<int> margin_colors_min, margin_colors_max;
    std::optional<uint64_t> backdrop_pieces_min, squares_hidden_min, squares_drawn_min, squares_drawn_max;
    std::optional<uint64_t> wide_field_frames_min, wide_field_fallbacks_max, wide_margin_polygons_min, wide_margin_polygons_max;
    int max_hash_distance = 24;
    std::vector<RamCheck> ram;
    bool screenshot = false;
    std::optional<double> audio_min_rms, audio_max_rms;
    int audio_frames = 60;
    bool wav = false;
};

// [[margin_watch]]: every video frame whose script frame lies in [from, to] (a wait holds the script frame, so one wait
// is covered by `from = to = its at`), the GPU renderer's picture is rendered and its outer columns looked at: the
// number of times they switch between black and lit from one frame to the next must not exceed `lit_changes_max`.
// `strip` writes a contact sheet of up to 48 consecutive frames around the first switch next to the check screenshots.
struct MarginWatch {
    int64_t from = 0, to = 0;
    std::string label;
    int lit_changes_max = 0;
    bool strip = false;
};

// [[flicker_watch]]: every video frame whose script frame lies in [from, to] is rendered by the GPU renderer and compared
// with the two different frames before it (a frame equal to the one before is the game showing a picture twice). A
// pixel flickers when the middle picture differs from both neighbours although those two agree. The picture is cut into
// 16x16 blocks (of its 640x360 grey version); a block flickers when a quarter of its pixels do. For every triple of
// different pictures the number of flickering blocks must stay at or below `flicker_blocks_max`. Characters moving and
// fine texture noise do not flicker whole blocks; a background that jumps for a frame and back (vertices falling back
// to integer positions, wrong depths) does.
struct FlickerWatch {
    int64_t from = 0, to = 0;
    std::string label;
    int flicker_blocks_max = 40;
};

// [[poke]]
struct Poke {
    int64_t at = 0;
    uint32_t addr = 0;
    int size = 4;
    uint32_t value = 0;
};

// [[savestate]] / [[loadstate]]
struct StateOp {
    int64_t at = 0;
    std::string path;
    bool replay = false;  // loadstate only
};

struct Scenario {
    std::string name;
    std::string description;
    int64_t frames = 0;
    bool skip_intro = true;
    std::string memcard = "none";
    std::vector<std::string> settings;  // "id=value"
    bool gpu = false;  // draw with a headless GPU renderer (implied by a widescreen setting)
    std::vector<Press> presses;
    std::vector<int64_t> reboots;  // sorted, within (0, frames)
    std::vector<Poke> pokes;  // sorted by `at`
    std::vector<StateOp> saves, loads;  // sorted by `at`
    std::string start_state;
    int64_t start_frame = 0;
    std::vector<Wait> waits;  // sorted by `at`
    std::vector<Check> checks;  // sorted by `at`
    std::vector<MarginWatch> margin_watches;
    std::vector<FlickerWatch> flicker_watches;
};

constexpr int64_t kIntroSkipFrame = 300;

// Parses a scenario file, merging its `include` files. Throws std::runtime_error with a readable message on errors.
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
    using RamReader = dw3::scenario::RamReader;
    explicit Timeline(const Scenario& s) : s_(&s) {}

    // Continues from script frame `script` at real frame `real` (after a state was loaded).
    void seek(int64_t real, int64_t script) {
        real_ = real;
        script_ = script;
        offset_ = real - script;
        waiting_ = false;
        steer_ = 0;
    }

    void advance(int64_t real_frame, const RamReader& read);
    int64_t script_frame() const { return script_; }
    uint16_t buttons() const;
    bool waiting() const { return waiting_; }
    // Set once a wait ran out of time; `failed_wait()` is that wait.
    bool timed_out() const { return timed_out_; }
    // Set once a pending wait's `guard` stopped holding (the walk left its map); `failed_wait()` is that wait.
    bool guard_failed() const { return guard_failed_; }
    bool failed() const { return timed_out_ || guard_failed_; }
    const Wait* failed_wait() const { return failed() ? &s_->waits[next_] : nullptr; }
    // Real frames the most recently finished wait took (0 before the first).
    int64_t last_wait_frames() const { return last_wait_frames_; }
    std::size_t completed_waits() const { return next_; }

private:
    bool satisfied(const Wait& w, const RamReader& read) const;
    const Scenario* s_;
    std::size_t next_ = 0;  // index of the pending or active wait
    bool waiting_ = false, timed_out_ = false, guard_failed_ = false;
    uint16_t steer_ = 0;  // d-pad buttons chosen for the current frame
    int64_t offset_ = 0, script_ = 0, real_ = 0, wait_start_ = 0, last_wait_frames_ = 0;
};

// 256-bit perceptual hash: image box-downscaled to 16x16 luma, each bit = cell brighter than mean.
std::array<uint64_t, 4> perceptual_hash(const uint32_t* rgba, int width, int height);
int hash_distance(const std::array<uint64_t, 4>& a, const std::array<uint64_t, 4>& b);
std::string hash_to_hex(const std::array<uint64_t, 4>& h);
std::optional<std::array<uint64_t, 4>> hash_from_hex(const std::string& hex);

}  // namespace dw3::scenario
