// SPU DSP building blocks. See spu_dsp.h for references.

#include "dw3/spu_dsp.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace dw3::spu_dsp {

void decode_adpcm_block(const uint8_t* b, int16_t* out, int32_t& s1, int32_t& s2) {
    int shift = b[0] & 0xF;
    if (shift > 12) shift = 9;
    int filter = std::min((b[0] >> 4) & 7, 4);
    const int32_t f0 = kAdpcmPos[static_cast<std::size_t>(filter)], f1 = kAdpcmNeg[static_cast<std::size_t>(filter)];
    for (int i = 0; i < 28; ++i) {
        unsigned nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t t = static_cast<int16_t>(nib << 12) >> shift;
        int32_t s = clamp16(t + ((s1 * f0 + s2 * f1 + 32) >> 6));
        out[i] = static_cast<int16_t>(s);
        s2 = s1;
        s1 = s;
    }
}

int16_t gauss_interpolate(const int16_t* w, unsigned idx) {
    idx &= 0xFF;
    int32_t out = (kGauss[0xFF - idx] * w[0]) >> 15;
    out += (kGauss[0x1FF - idx] * w[1]) >> 15;
    out += (kGauss[0x100 + idx] * w[2]) >> 15;
    out += (kGauss[idx] * w[3]) >> 15;
    return clamp16(out);
}

void envelope_tick(int32_t& level, int32_t& counter, const EnvRate& r) {
    const int shift = r.shift;
    int32_t step = 7 - r.step;
    if (r.decreasing != r.phase_negative) step = ~step;  // +7..+4 -> -8..-5
    step *= 1 << std::max(0, 11 - shift);
    int32_t inc = 0x8000 >> std::max(0, shift - 11);
    if (r.exponential && !r.decreasing && level > 0x6000) {
        if (shift < 10) step /= 4;
        else if (shift >= 11) inc /= 4;
        else {
            step /= 2;
            inc /= 2;
        }
    } else if (r.exponential && r.decreasing) {
        step = (step * level) >> 15;
    }
    if (!r.never_steps) inc = std::max(inc, 1);
    counter += inc;
    if (!(counter & 0x8000)) return;
    counter = 0;
    level += step;
    if (!r.decreasing) level = std::clamp(level, -0x8000, 0x7FFF);
    else if (r.phase_negative) level = std::clamp(level, -0x8000, 0);
    else level = std::max(level, 0);
}

void Noise::tick(unsigned shift, unsigned step) {
    timer -= static_cast<int32_t>(step + 4);
    unsigned parity = ((level >> 15) ^ (level >> 12) ^ (level >> 11) ^ (level >> 10) ^ 1u) & 1u;
    if (timer < 0) {
        level = static_cast<uint16_t>(level * 2 + parity);
        timer += 0x20000 >> shift;
        if (timer < 0) timer += 0x20000 >> shift;
    }
}

// ---- Reverb ----

int16_t Reverb::rd(const uint8_t* ram, int32_t off) const {
    int64_t size = 0x80000 - static_cast<int64_t>(start_);
    int64_t d = (static_cast<int64_t>(cur_) + off - start_) % size;
    if (d < 0) d += size;
    uint32_t a = static_cast<uint32_t>(start_ + d);
    return static_cast<int16_t>(ram[a] | (ram[a + 1] << 8));
}

void Reverb::wr(uint8_t* ram, int32_t off, int16_t v) const {
    int64_t size = 0x80000 - static_cast<int64_t>(start_);
    int64_t d = (static_cast<int64_t>(cur_) + off - start_) % size;
    if (d < 0) d += size;
    uint32_t a = static_cast<uint32_t>(start_ + d);
    ram[a] = static_cast<uint8_t>(v);
    ram[a + 1] = static_cast<uint8_t>(v >> 8);
}

void Reverb::compute(uint8_t* ram, const uint16_t* r, bool write, int16_t xl, int16_t xr, int16_t& ol, int16_t& or_) {
    auto mul = [](int32_t a, int32_t b) { return (a * b) >> 15; };
    auto vol = [&](Reg i) { return static_cast<int32_t>(static_cast<int16_t>(r[i])); };
    auto adr = [&](Reg i) { return static_cast<int32_t>(r[i]) * 8; };
    const int32_t lin = mul(vol(vLIN), xl), rin = mul(vol(vRIN), xr);
    if (write) {
        auto stage = [&](Reg m, int32_t in, int32_t src_off) {
            int32_t prev = rd(ram, adr(m) - 2);
            int32_t t = in + mul(rd(ram, src_off), vol(vWALL)) - prev;
            wr(ram, adr(m), clamp16(mul(t, vol(vIIR)) + prev));
        };
        stage(mLSAME, lin, adr(dLSAME));
        stage(mRSAME, rin, adr(dRSAME));
        stage(mLDIFF, lin, adr(dRDIFF));
        stage(mRDIFF, rin, adr(dLDIFF));
    }
    auto side = [&](Reg c1, Reg c2, Reg c3, Reg c4, Reg a1, Reg a2) {
        int32_t o = mul(vol(vCOMB1), rd(ram, adr(c1))) + mul(vol(vCOMB2), rd(ram, adr(c2))) +
                    mul(vol(vCOMB3), rd(ram, adr(c3))) + mul(vol(vCOMB4), rd(ram, adr(c4)));
        o = clamp16(o);
        for (auto [m, v, d] : {std::tuple{a1, vAPF1, dAPF1}, std::tuple{a2, vAPF2, dAPF2}}) {
            int32_t src = rd(ram, adr(m) - adr(d));
            o = clamp16(o - mul(vol(v), src));
            if (write) {
                wr(ram, adr(m), static_cast<int16_t>(o));
                src = rd(ram, adr(m) - adr(d));  // re-read after the write (matters only when the displacement is 0)
            }
            o = clamp16(mul(o, vol(v)) + src);
        }
        return static_cast<int16_t>(o);
    };
    ol = side(mLCOMB1, mLCOMB2, mLCOMB3, mLCOMB4, mLAPF1, mLAPF2);
    or_ = side(mRCOMB1, mRCOMB2, mRCOMB3, mRCOMB4, mRAPF1, mRAPF2);
    cur_ = (cur_ + 2) & 0x7FFFE;
    if (cur_ < start_) cur_ = start_;
}

void Reverb::tick(uint8_t* ram, const uint16_t* regs, bool write, int16_t in_l, int16_t in_r, int16_t& out_l,
                  int16_t& out_r) {
    in_hist_[0][in_pos_ & 63] = in_l;
    in_hist_[1][in_pos_ & 63] = in_r;
    ++in_pos_;
    if (phase_ == 0) {
        int32_t y[2] = {0, 0};
        for (unsigned k = 0; k < 39; ++k) {
            if (!kFir[k]) continue;
            for (unsigned c = 0; c < 2; ++c) y[c] += kFir[k] * in_hist_[c][(in_pos_ - 1 - k) & 63];
        }
        int16_t ol = 0, or_ = 0;
        compute(ram, regs, write, clamp16(y[0] >> 15), clamp16(y[1] >> 15), ol, or_);
        out_hist_[0][out_pos_ & 31] = ol;
        out_hist_[1][out_pos_ & 31] = or_;
        ++out_pos_;
        int32_t z[2] = {0, 0};
        for (unsigned j = 0; j < 20; ++j)
            for (unsigned c = 0; c < 2; ++c) z[c] += kFir[2 * j] * out_hist_[c][(out_pos_ - 1 - j) & 31];
        out_l = clamp16(z[0] >> 14);
        out_r = clamp16(z[1] >> 14);
    } else {
        out_l = out_hist_[0][(out_pos_ - 1 - 9) & 31];
        out_r = out_hist_[1][(out_pos_ - 1 - 9) & 31];
    }
    phase_ ^= 1;
}

}  // namespace dw3::spu_dsp
