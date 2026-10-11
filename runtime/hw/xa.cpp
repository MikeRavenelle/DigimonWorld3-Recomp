// XA-ADPCM sector decoder and resampler. See xa.h for references.

#include "dw3/xa.h"

#include <algorithm>
#include <vector>

#include "dw3/spu_dsp.h"

namespace dw3 {

using namespace spu_dsp;

void XaDecoder::reset() {
    fifo_.clear();
    s1_ = s2_ = {};
    ring_ = {};
    pos_ = 0;
    sixstep_ = 6;
}

std::size_t XaDecoder::decode_sector(const uint8_t* raw, std::array<std::vector<int16_t>, 2>& out, bool& stereo,
                                     bool& half_rate) {
    const uint8_t coding = raw[19];
    stereo = coding & 1;
    half_rate = coding & 4;
    const bool eight = coding & 0x10;
    const unsigned units = eight ? 4 : 8;
    out[0].clear();
    out[1].clear();
    const uint8_t* group = raw + 24;
    for (int g = 0; g < 18; ++g, group += 128) {
        for (unsigned u = 0; u < units; ++u) {
            const unsigned ch = stereo ? (u & 1) : 0;
            const uint8_t hdr = group[4 + u];
            int range = hdr & 0xF;
            if (range > 12) range = 9;
            const int filter = std::min((hdr >> 4) & 3, 3);
            const int32_t f0 = kAdpcmPos[static_cast<std::size_t>(filter)];
            const int32_t f1 = kAdpcmNeg[static_cast<std::size_t>(filter)];
            for (unsigned j = 0; j < 28; ++j) {
                int32_t t;
                if (eight) t = static_cast<int8_t>(group[16 + j * 4 + u]) * 256;
                else t = static_cast<int16_t>(((group[16 + j * 4 + (u >> 1)] >> ((u & 1) * 4)) & 0xF) << 12);
                t >>= range;
                int32_t s = clamp16(t + ((s1_[ch] * f0 + s2_[ch] * f1 + 32) >> 6));
                s2_[ch] = s1_[ch];
                s1_[ch] = s;
                out[ch].push_back(static_cast<int16_t>(s));
            }
        }
    }
    return out[0].size();
}

void XaDecoder::resample_push(int16_t l, int16_t r) {
    ring_[0][pos_ & 31] = l;
    ring_[1][pos_ & 31] = r;
    ++pos_;
    if (--sixstep_ != 0) return;
    sixstep_ = 6;
    for (std::size_t t = 0; t < 7; ++t) {
        int16_t o[2];
        for (unsigned c = 0; c < 2; ++c) {
            int32_t sum = 0;
            for (unsigned i = 1; i <= 29; ++i) sum += (ring_[c][(pos_ - i) & 31] * kZigzag[t][i - 1]) / 0x8000;
            o[c] = clamp16(sum);
        }
        fifo_.push(o[0], o[1]);
    }
}

void XaDecoder::xa_sector(const uint8_t* raw) {
    std::array<std::vector<int16_t>, 2> ch;
    bool stereo, half;
    std::size_t n = decode_sector(raw, ch, stereo, half);
    for (std::size_t i = 0; i < n; ++i) {
        int16_t l = ch[0][i], r = stereo ? ch[1][i] : l;
        resample_push(l, r);
        if (half) resample_push(l, r);
    }
    ++sectors_;
}

bool XaDecoder::next_frame(int16_t& l, int16_t& r) {
    int16_t f[2];
    if (fifo_.size() == 0 || fifo_.pop(f) == 0) {
        l = r = 0;
        return false;
    }
    if (cd_ && cd_->adpcm_muted()) {
        l = r = 0;
        return true;
    }
    int32_t ll = 0x80, lr = 0, rl = 0, rr = 0x80;
    if (cd_) {
        const auto& v = cd_->volume();
        ll = v.ll;
        lr = v.lr;
        rl = v.rl;
        rr = v.rr;
    }
    l = clamp16((f[0] * ll + f[1] * rl) >> 7);
    r = clamp16((f[0] * lr + f[1] * rr) >> 7);
    return true;
}

}  // namespace dw3
