#pragma once

// PS1 SPU (0x1F801C00-0x1F801FFF, 512 KiB SPU RAM, DMA channel 4): register file, SPUSTAT/SPUCNT handshakes,
// manual and DMA transfers, IRQ9, and the sample engine: 24 ADPCM voices (Gaussian interpolation, pitch
// modulation, noise, ADSR, sweep volumes), CD/XA input, capture buffers, reverb and the main mixer.
//
// Samples are generated from emulated time: once start() hands the SPU a Scheduler, a periodic event produces
// batches of 44.1 kHz frames (768 CPU cycles each) into an AudioRing, and key-on/off writes first catch the
// engine up to the current cycle so they land on the right sample. Without start() the SPU is only a register
// file (unit tests); call run_to() to generate by hand.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "dw3/audio_ring.h"
#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/spu_dsp.h"

namespace dw3 {

// CD audio input to the SPU mixer (44.1 kHz stereo, after the CD controller's volume matrix / mute).
struct CdAudioSource {
    virtual ~CdAudioSource() = default;
    virtual bool next_frame(int16_t& l, int16_t& r) = 0;  // false: nothing queued (silence)
};

// Which part of the game a voice belongs to, so that the host can mix them at different levels.
enum class VoiceGroup : uint8_t { Music, Effects };

// Decides the group of a voice at the moment it is keyed on. The SPU itself cannot tell: that depends on how the game
// drives it, which the host reads from the game's memory.
struct VoiceClassifier {
    virtual ~VoiceClassifier() = default;
    virtual VoiceGroup classify(int voice) = 0;
};

class Spu : public DmaDevice {
public:
    static constexpr uint32_t kBase = 0x1F801C00u;
    static constexpr uint32_t kSize = 0x400;
    static constexpr uint32_t kRamSize = 512 * 1024;
    static constexpr int kVoices = 24;
    static constexpr int64_t kCyclesPerSample = 768;  // 33.8688 MHz / 44.1 kHz
    static constexpr int kBatchSamples = 256;

    // Register offsets from kBase.
    static constexpr uint32_t kMainVolL = 0x180, kMainVolR = 0x182, kReverbVolL = 0x184, kReverbVolR = 0x186;
    static constexpr uint32_t kKeyOn = 0x188, kKeyOff = 0x18C, kPitchMod = 0x190, kNoiseOn = 0x194, kReverbOn = 0x198;
    static constexpr uint32_t kEndx = 0x19C, kReverbStart = 0x1A2;
    static constexpr uint32_t kIrqAddr = 0x1A4, kTransferAddr = 0x1A6, kTransferFifo = 0x1A8;
    static constexpr uint32_t kSpuCnt = 0x1AA, kTransferCtrl = 0x1AC, kSpuStat = 0x1AE;
    static constexpr uint32_t kCdVolL = 0x1B0, kCdVolR = 0x1B2, kMainVolXL = 0x1B8, kMainVolXR = 0x1BA;
    static constexpr uint32_t kReverbRegs = 0x1C0, kVoiceVolX = 0x200;

    Spu(Bus& bus, IrqController& irq, Dma& dma);

    uint16_t read16(uint32_t offset);
    void write16(uint32_t offset, uint16_t value);

    // DmaDevice (channel 4): RAM -> SPU RAM writes and SPU RAM -> RAM reads at the transfer address.
    void dma_write(std::span<const uint32_t> words) override;
    void dma_read(std::span<uint32_t> out) override;

    // Host mixing by voice group (not guest state, so not saved): percent 0..100 of each group's level, applied before
    // reverb. CD/XA audio follows the music group. Without a classifier every voice is music.
    void set_voice_classifier(VoiceClassifier* classifier) {
        classifier_ = classifier;
        groups_stale_ = true;
    }
    void set_group_volume(VoiceGroup group, int percent) {
        group_gain_[static_cast<std::size_t>(group)] = std::clamp(percent, 0, 100) * 256 / 100;
    }

    // Starts timed sample generation driven by the scheduler.
    void start(Scheduler& sched);
    void set_cd_source(CdAudioSource* src) { cd_ = src; }
    // Generates every sample due up to CPU cycle `now` (no-op if already there).
    void run_to(int64_t now);
    // Produces exactly one 44.1 kHz frame into audio() regardless of time (tests, offline rendering).
    void generate_sample();

    AudioRing& audio() { return audio_; }
    // Drains up to out.size()/2 stereo frames; returns the number of frames written.
    std::size_t drain_samples(std::span<int16_t> out) { return audio_.pop(out); }
    uint64_t frames_generated() const { return frames_; }

    std::span<const uint8_t> ram() const { return ram_; }
    std::span<uint8_t> ram_mut() { return ram_; }
    uint16_t reg(uint32_t offset) const { return regs_[(offset & (kSize - 1)) >> 1]; }
    uint32_t transfer_address() const { return transfer_addr_; }
    uint32_t voices_on() const { return key_on_; }  // key-on bitmask (cleared by key-off or a muting loop end)
    uint32_t endx() const { return endx_; }

    // Voice introspection (tests / debugging).
    enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release };
    Phase voice_phase(int v) const { return voices_[static_cast<std::size_t>(v)].phase; }
    int32_t voice_level(int v) const { return voices_[static_cast<std::size_t>(v)].level; }
    int16_t voice_output(int v) const { return voices_[static_cast<std::size_t>(v)].out; }
    uint32_t voice_address(int v) const { return voices_[static_cast<std::size_t>(v)].addr; }
    uint32_t voice_counter(int v) const { return voices_[static_cast<std::size_t>(v)].counter; }
    int32_t voice_volume(int v, int side) const {
        return voices_[static_cast<std::size_t>(v)].vol[side & 1].level;
    }
    int32_t main_volume(int side) const { return main_vol_[side & 1].level; }
    const spu_dsp::Noise& noise() const { return noise_; }
    const spu_dsp::Reverb& reverb() const { return reverb_; }

    // Save state (see state.h). Covers SPU RAM, registers, voices, mixer, noise, reverb, capture position, the
    // sample clock and the pending batch event. Samples already queued in audio() are not state: loading clears
    // them. When the state was saved from a started SPU, load into one on which start() has already been called
    // (with the Scheduler whose own state is loaded first); a state without timed generation loads either way.
    void io(StateIo& s);

private:
    // Volume with direct or sweep mode (voice L/R and main L/R).
    struct Sweep {
        uint16_t reg = 0;
        int32_t level = 0;
        int32_t counter = 0;
        void write(uint16_t v) {
            reg = v;
            if (!(v & 0x8000)) level = static_cast<int16_t>(static_cast<uint16_t>(v << 1));
        }
        void tick();
    };
    struct Voice {
        uint32_t addr = 0, repeat = 0;  // byte addresses of the current and loop-start ADPCM blocks
        uint32_t counter = 0;           // pitch counter: bits 12+ = sample in block, bits 4-11 = interpolation index
        std::array<int16_t, 31> buf{};  // 3 history samples + the 28 decoded samples of the current block
        int32_t s1 = 0, s2 = 0;         // ADPCM decoder history
        uint8_t flags = 0;
        Phase phase = Phase::Off;
        int32_t level = 0, adsr_counter = 0;
        int16_t out = 0;  // after envelope, before volume (pitch-modulation source, capture)
        std::array<Sweep, 2> vol;
    };

    void write_ram16(uint16_t v);
    uint16_t read_ram16();
    void check_irq(uint32_t addr);
    void check_irq_range(uint32_t addr, uint32_t len);
    void sync() {
        if (sched_) run_to(sched_->now());
    }
    void key_on(uint32_t mask);
    void key_off(uint32_t mask);
    void decode_block(Voice& v);
    void next_block(Voice& v, int index);
    void envelope(Voice& v, uint16_t adsr1, uint16_t adsr2);
    void schedule_batch(int64_t at);
    Scheduler::Callback batch_callback();
    uint16_t vreg(int v, unsigned r) const { return regs_[static_cast<std::size_t>(v) * 8 + r]; }

    IrqController& irq_;
    Scheduler* sched_ = nullptr;
    CdAudioSource* cd_ = nullptr;
    std::array<uint16_t, kSize / 2> regs_{};
    std::vector<uint8_t> ram_;
    uint32_t transfer_addr_ = 0;  // byte address in SPU RAM
    uint32_t key_on_ = 0, endx_ = 0;

    std::array<Voice, kVoices> voices_;
    VoiceClassifier* classifier_ = nullptr;
    std::array<VoiceGroup, kVoices> groups_{};
    bool groups_stale_ = false;      // classify every voice again before the next sample (after a state load)
    std::array<int32_t, 2> group_gain_{256, 256};  // 256 is unity
    std::array<Sweep, 2> main_vol_;
    spu_dsp::Noise noise_;
    spu_dsp::Reverb reverb_;
    unsigned capture_pos_ = 0;
    int64_t next_cycle_ = 0;
    EventId batch_event_ = kNoEvent;
    uint64_t frames_ = 0;
    AudioRing audio_;
};

}  // namespace dw3
