#pragma once

// CD-ROM XA-ADPCM decoder: turns raw 2352-byte audio sectors into 44.1 kHz stereo frames for the SPU's CD input.
// Decoding and the 25-point zigzag 37.8 kHz -> 44.1 kHz resampler follow psx-spx "CDROM XA Audio ADPCM
// Compression" (https://psx-spx.consoledev.net/). 18.9 kHz streams repeat each sample before resampling.

#include <array>
#include <cstdint>

#include "dw3/audio_ring.h"
#include "dw3/cdrom.h"
#include "dw3/spu.h"
#include "dw3/state.h"

namespace dw3 {

class XaDecoder final : public XaSink, public CdAudioSource {
public:
    // `cd` supplies the CD->SPU volume matrix and ADPCM mute (null: unity, never muted).
    explicit XaDecoder(const Cdrom* cd = nullptr, std::size_t fifo_frames = 1u << 15) : cd_(cd), fifo_(fifo_frames) {}

    void xa_sector(const uint8_t* raw2352) override;
    // Pops one frame through the volume matrix; silence (false) when the FIFO is empty or ADPCM is muted.
    bool next_frame(int16_t& l, int16_t& r) override;

    std::size_t queued() const { return fifo_.size(); }
    AudioRing& fifo() { return fifo_; }
    void reset();

    // Decodes the sound groups of one sector into per-channel 37.8/18.9 kHz samples (stereo: L and R; mono: L only).
    // Decoder history persists in this object. Returns samples per channel produced.
    std::size_t decode_sector(const uint8_t* raw2352, std::array<std::vector<int16_t>, 2>& out, bool& stereo,
                              bool& half_rate);
    // Pushes one 37.8 kHz sample pair through the resampler (emits 7 frames per 6 inputs).
    void resample_push(int16_t l, int16_t r);

    uint64_t sectors() const { return sectors_; }

    // Save state: ADPCM history, resampler ring and phase, and the decoded frames not yet consumed by the SPU.
    void io(StateIo& s);

private:
    const Cdrom* cd_;
    AudioRing fifo_;
    std::array<int32_t, 2> s1_{}, s2_{};
    std::array<std::array<int16_t, 32>, 2> ring_{};
    unsigned pos_ = 0, sixstep_ = 6;
    uint64_t sectors_ = 0;
};

}  // namespace dw3
