// PS1 Geometry Transformation Engine (COP2).
//
// Behaviour follows psx-spx GTE documentation; cross-checked against Beetle PSX / Mednafen
// (GPL-2+) and PCSX-Redux (GPL-2+).
#include "dw3/gte.h"

#include "dw3/precise_vertex.h"

#include <algorithm>
#include <array>
#include <bit>

namespace dw3 {

namespace {

constexpr uint32_t flag_error_mask = 0x7F87E000u;
constexpr int64_t mac_max = (int64_t{1} << 43) - 1;
constexpr int64_t mac_min = -(int64_t{1} << 43);

// Reciprocal seed table used by the hardware divider (psx-spx "GTE Division Inaccuracy").
constexpr std::array<uint8_t, 257> make_unr_table() {
    std::array<uint8_t, 257> t{};
    for (int i = 0; i <= 0x100; ++i) {
        int v = ((0x40000 / (i + 0x100) + 1) / 2) - 0x101;
        t[static_cast<size_t>(i)] = static_cast<uint8_t>(v < 0 ? 0 : v);
    }
    return t;
}
constexpr std::array<uint8_t, 257> unr_table = make_unr_table();

constexpr int16_t s16(uint32_t v) { return static_cast<int16_t>(v & 0xFFFFu); }

// Command-word field accessors.
constexpr unsigned cmd_op(uint32_t c) { return c & 0x3Fu; }

}  // namespace

// Command executor; a friend of Gte so it can poke the raw register files.
struct GteImpl {
    Gte& g;
    uint32_t flag = 0;
    int sh = 0;
    bool lm = false;
    uint32_t code = 0;

    explicit GteImpl(Gte& gte) : g(gte) {}

    // ---- register access ----
    uint32_t& d(int i) { return g.data_[i]; }
    uint32_t c(int i) const { return g.ctrl_[i]; }
    int32_t ci(int i) const { return static_cast<int32_t>(g.ctrl_[i]); }
    int32_t ir(int i) { return static_cast<int32_t>(g.data_[8 + i]); }  // i = 0..3
    int32_t mac(int i) { return static_cast<int32_t>(g.data_[24 + i]); }
    int ctrl_s16(int i) const { return s16(g.ctrl_[i]); }

    void get_mat(int base, int16_t m[9]) const {
        for (int k = 0; k < 9; ++k) {
            uint32_t w = g.ctrl_[base + k / 2];
            m[k] = static_cast<int16_t>((k & 1) ? (w >> 16) : (w & 0xFFFF));
        }
    }
    void get_cv(int base, int32_t v[3]) const {
        for (int k = 0; k < 3; ++k) v[k] = ci(base + k);
    }
    void get_vec(int n, int32_t v[3]) {
        uint32_t xy = g.data_[n * 2];
        v[0] = s16(xy);
        v[1] = s16(xy >> 16);
        v[2] = s16(g.data_[n * 2 + 1]);
    }
    void get_ir_vec(int32_t v[3]) {
        for (int k = 0; k < 3; ++k) v[k] = ir(k + 1);
    }

    // ---- saturation helpers ----
    // 44-bit MAC1-3 overflow check; returns the value wrapped to 44 bits.
    int64_t chk(int i, int64_t x) {
        if (x > mac_max) flag |= 1u << (31 - i);
        else if (x < mac_min) flag |= 1u << (28 - i);
        return static_cast<int64_t>(static_cast<uint64_t>(x) << 20) >> 20;
    }
    // 32-bit MAC0 overflow check.
    int64_t chk0(int64_t x) {
        if (x > INT32_MAX) flag |= 1u << 16;
        else if (x < INT32_MIN) flag |= 1u << 15;
        return x;
    }
    void set_mac(int i, int64_t v) { d(24 + i) = static_cast<uint32_t>(static_cast<int32_t>(v >> sh)); }
    void set_mac0(int64_t v) { d(24) = static_cast<uint32_t>(v); }

    int32_t clamp_ir(int i, int32_t v, bool lmode, bool set_flag = true) {
        int32_t lo = lmode ? 0 : -0x8000;
        if (v < lo) {
            v = lo;
            if (set_flag) flag |= 1u << (25 - i);
        } else if (v > 0x7FFF) {
            v = 0x7FFF;
            if (set_flag) flag |= 1u << (25 - i);
        }
        return v;
    }
    void set_ir(int i, int32_t v) { d(8 + i) = static_cast<uint32_t>(clamp_ir(i, v, lm)); }

    void set_macs_irs(const int64_t out[3]) {
        for (int i = 0; i < 3; ++i) {
            set_mac(i + 1, out[i]);
            set_ir(i + 1, mac(i + 1));
        }
    }

    void push_color() {
        uint32_t px = 0;
        for (int i = 0; i < 3; ++i) {
            int32_t v = mac(i + 1) >> 4;
            if (v < 0) {
                v = 0;
                flag |= 1u << (21 - i);
            } else if (v > 255) {
                v = 255;
                flag |= 1u << (21 - i);
            }
            px |= static_cast<uint32_t>(v) << (8 * i);
        }
        d(20) = d(21);
        d(21) = d(22);
        d(22) = px | (code << 24);
    }

    void push_sz(int64_t v) {
        if (v < 0) {
            v = 0;
            flag |= 1u << 18;
        } else if (v > 0xFFFF) {
            v = 0xFFFF;
            flag |= 1u << 18;
        }
        d(16) = d(17);
        d(17) = d(18);
        d(18) = d(19);
        d(19) = static_cast<uint32_t>(v);
    }

    void push_sxy(int32_t x, int32_t y) {
        d(12) = d(13);
        d(13) = d(14);
        d(14) = (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) << 16);
    }

    // ---- shared arithmetic ----
    // out = (cv << 12) + M * v, checking each partial sum. fc_bug reproduces the far-colour MVMVA hardware bug.
    void mat_vec(const int16_t m[9], const int32_t v[3], const int32_t* cv, bool fc_bug, int64_t out[3]) {
        for (int i = 0; i < 3; ++i) {
            int64_t t = cv ? static_cast<int64_t>(cv[i]) * 4096 : 0;
            if (fc_bug) {
                int64_t x = chk(i + 1, t + static_cast<int64_t>(m[i * 3]) * v[0]);
                clamp_ir(i + 1, static_cast<int32_t>(x >> sh), false);  // flags only
                t = chk(i + 1, static_cast<int64_t>(m[i * 3 + 1]) * v[1]);
                t = chk(i + 1, t + static_cast<int64_t>(m[i * 3 + 2]) * v[2]);
            } else {
                t = chk(i + 1, t + static_cast<int64_t>(m[i * 3]) * v[0]);
                t = chk(i + 1, t + static_cast<int64_t>(m[i * 3 + 1]) * v[1]);
                t = chk(i + 1, t + static_cast<int64_t>(m[i * 3 + 2]) * v[2]);
            }
            out[i] = t;
        }
    }

    // base[i] + IR0 * sat16(((FC << 12) - base[i]) >> sh)
    void interpolate(const int64_t base[3]) {
        int64_t out[3];
        for (int i = 0; i < 3; ++i) {
            int64_t diff = chk(i + 1, static_cast<int64_t>(ci(21 + i)) * 4096 - base[i]);
            int32_t t = clamp_ir(i + 1, static_cast<int32_t>(diff >> sh), false);
            out[i] = chk(i + 1, base[i] + static_cast<int64_t>(ir(0)) * t);
        }
        set_macs_irs(out);
    }

    uint32_t divide(uint32_t h, uint32_t sz3) {
        if (h >= sz3 * 2) {
            flag |= 1u << 17;
            return 0x1FFFF;
        }
        int z = std::countl_zero(static_cast<uint16_t>(sz3));
        uint64_t n = static_cast<uint64_t>(h) << z;
        uint64_t dd = static_cast<uint64_t>(sz3) << z;
        int64_t u = unr_table[(dd - 0x7FC0) >> 7] + 0x101;
        int64_t r = (0x2000080 - static_cast<int64_t>(dd) * u) >> 8;
        r = (0x80 + r * u) >> 8;
        uint64_t res = (n * static_cast<uint64_t>(r) + 0x8000) >> 16;
        return static_cast<uint32_t>(std::min<uint64_t>(res, 0x1FFFF));
    }

    // ---- commands ----
    void rtp(int v, bool last) {
        int16_t m[9];
        int32_t vec[3], tr[3];
        int64_t out[3];
        get_mat(0, m);
        get_vec(v, vec);
        get_cv(5, tr);
        mat_vec(m, vec, tr, false, out);
        set_mac(1, out[0]);
        set_mac(2, out[1]);
        set_mac(3, out[2]);
        set_ir(1, mac(1));
        set_ir(2, mac(2));
        // IR3 flag is derived from the unshifted (>> 12) value, the stored value still honours lm.
        int32_t z12 = static_cast<int32_t>(out[2] >> 12);
        if (z12 < -0x8000 || z12 > 0x7FFF) flag |= 1u << 22;
        d(11) = static_cast<uint32_t>(clamp_ir(3, mac(3), lm, false));
        push_sz(out[2] >> 12);

        const bool div_overflow = (c(26) & 0xFFFF) >= d(19) * 2;
        uint32_t q = divide(c(26) & 0xFFFF, d(19));
        int64_t m0x = chk0(static_cast<int64_t>(q) * ir(1) + ci(24));
        int64_t m0y = chk0(static_cast<int64_t>(q) * ir(2) + ci(25));
        int32_t sx = static_cast<int32_t>(m0x >> 16), sy = static_cast<int32_t>(m0y >> 16);
        bool clamped = false;
        if (sx < -0x400 || sx > 0x3FF) {
            sx = std::clamp(sx, -0x400, 0x3FF);
            flag |= 1u << 14;
            clamped = true;
        }
        if (sy < -0x400 || sy > 0x3FF) {
            sy = std::clamp(sy, -0x400, 0x3FF);
            flag |= 1u << 13;
            clamped = true;
        }
        push_sxy(sx, sy);
        // The truncated words above are what the game sees; the fractions behind them are kept for the renderer.
        // Only a clean projection is recorded: no saturation, no divide overflow, no MAC0 overflow.
        if (g.precise_ && !clamped && !div_overflow && m0x >= INT32_MIN && m0x <= INT32_MAX && m0y >= INT32_MIN &&
            m0y <= INT32_MAX)
            g.precise_->record(sx, sy, static_cast<double>(m0x) / 65536.0, static_cast<double>(m0y) / 65536.0,
                               static_cast<double>(d(19)));
        if (last) {
            int64_t m0 = chk0(static_cast<int64_t>(q) * ctrl_s16(27) + ci(28));
            set_mac0(m0);
            int32_t i0 = static_cast<int32_t>(m0 >> 12);
            if (i0 < 0 || i0 > 0x1000) {
                i0 = std::clamp(i0, 0, 0x1000);
                flag |= 1u << 12;
            }
            d(8) = static_cast<uint32_t>(i0);
        }
    }

    void nclip() {
        auto sx = [&](int n) { return static_cast<int64_t>(s16(d(12 + n))); };
        auto sy = [&](int n) { return static_cast<int64_t>(s16(d(12 + n) >> 16)); };
        int64_t v = sx(0) * sy(1) + sx(1) * sy(2) + sx(2) * sy(0) - sx(0) * sy(2) - sx(1) * sy(0) - sx(2) * sy(1);
        set_mac0(chk0(v));
    }

    void op() {
        int64_t d1 = ctrl_s16(0), d2 = s16(g.ctrl_[2]), d3 = s16(g.ctrl_[4]);
        int64_t i1 = ir(1), i2 = ir(2), i3 = ir(3);
        int64_t out[3] = {chk(1, i3 * d2 - i2 * d3), chk(2, i1 * d3 - i3 * d1), chk(3, i2 * d1 - i1 * d2)};
        set_macs_irs(out);
    }

    void color_from(const int64_t base[3]) { interpolate(base); }

    void dpc(uint32_t rgb) {
        int64_t base[3];
        for (int i = 0; i < 3; ++i) base[i] = static_cast<int64_t>((rgb >> (8 * i)) & 0xFF) << 16;
        interpolate(base);
        push_color();
    }

    void intpl() {
        int64_t base[3];
        for (int i = 0; i < 3; ++i) base[i] = static_cast<int64_t>(ir(i + 1)) * 4096;
        interpolate(base);
        push_color();
    }

    void mvmva() {
        unsigned mx = (g.cmd_ >> 17) & 3, vx = (g.cmd_ >> 15) & 3, cvs = (g.cmd_ >> 13) & 3;
        int16_t m[9];
        if (mx == 3) {
            int16_t r4 = static_cast<int16_t>(((d(6) & 0xFF) << 4));
            int16_t rt13 = s16(g.ctrl_[1]), rt22 = s16(g.ctrl_[2]);
            int16_t gm[9] = {static_cast<int16_t>(-r4), r4, static_cast<int16_t>(ir(0)), rt13, rt13, rt13, rt22, rt22, rt22};
            std::copy(gm, gm + 9, m);
        } else {
            get_mat(mx == 0 ? 0 : (mx == 1 ? 8 : 16), m);
        }
        int32_t v[3], cv[3] = {0, 0, 0};
        if (vx == 3) get_ir_vec(v);
        else get_vec(static_cast<int>(vx), v);
        if (cvs != 3) get_cv(cvs == 0 ? 5 : (cvs == 1 ? 13 : 21), cv);
        int64_t out[3];
        mat_vec(m, v, cvs == 3 ? nullptr : cv, cvs == 2, out);
        set_macs_irs(out);
    }

    // Light matrix then colour matrix stage shared by NC*.
    void light(int vn) {
        int16_t m[9];
        int32_t v[3];
        int64_t out[3];
        get_mat(8, m);
        get_vec(vn, v);
        mat_vec(m, v, nullptr, false, out);
        set_macs_irs(out);
        color_matrix();
    }
    void color_matrix() {
        int16_t m[9];
        int32_t v[3], bk[3];
        int64_t out[3];
        get_mat(16, m);
        get_ir_vec(v);
        get_cv(13, bk);
        mat_vec(m, v, bk, false, out);
        set_macs_irs(out);
    }
    void color_base(int64_t base[3]) {
        for (int i = 0; i < 3; ++i) base[i] = chk(i + 1, static_cast<int64_t>((d(6) >> (8 * i)) & 0xFF) * 16 * ir(i + 1));
    }
    void color_mult() {
        int64_t base[3];
        color_base(base);
        set_macs_irs(base);
        push_color();
    }
    void color_interp() {
        int64_t base[3];
        color_base(base);
        interpolate(base);
        push_color();
    }

    void sqr() {
        int64_t out[3];
        for (int i = 0; i < 3; ++i) out[i] = chk(i + 1, static_cast<int64_t>(ir(i + 1)) * ir(i + 1));
        set_macs_irs(out);
    }

    void avsz(bool four) {
        int64_t sum = static_cast<int64_t>(d(17) & 0xFFFF) + (d(18) & 0xFFFF) + (d(19) & 0xFFFF);
        int64_t zsf;
        if (four) {
            sum += d(16) & 0xFFFF;
            zsf = ctrl_s16(30);
        } else {
            zsf = ctrl_s16(29);
        }
        int64_t m0 = chk0(zsf * sum);
        set_mac0(m0);
        int64_t o = m0 >> 12;
        if (o < 0 || o > 0xFFFF) {
            o = std::clamp<int64_t>(o, 0, 0xFFFF);
            flag |= 1u << 18;
        }
        d(7) = static_cast<uint32_t>(o);
    }

    void gpf(bool accumulate) {
        int64_t out[3];
        for (int i = 0; i < 3; ++i) {
            int64_t b = accumulate ? static_cast<int64_t>(mac(i + 1)) * (int64_t{1} << sh) : 0;
            out[i] = chk(i + 1, b + static_cast<int64_t>(ir(i + 1)) * ir(0));
        }
        set_macs_irs(out);
        push_color();
    }

    void run(uint32_t cmd) {
        sh = (cmd & (1u << 19)) ? 12 : 0;
        lm = (cmd & (1u << 10)) != 0;
        code = d(6) >> 24;
        switch (cmd_op(cmd)) {
            case 0x01: rtp(0, true); break;
            case 0x06: nclip(); break;
            case 0x0C: op(); break;
            case 0x10: dpc(d(6)); break;
            case 0x11: intpl(); break;
            case 0x12: mvmva(); break;
            case 0x13: light(0); color_interp(); break;
            case 0x14: color_matrix(); color_interp(); break;
            case 0x16:
                for (int v = 0; v < 3; ++v) {
                    light(v);
                    color_interp();
                }
                break;
            case 0x1B: light(0); color_mult(); break;
            case 0x1C: color_matrix(); color_mult(); break;
            case 0x1E: light(0); push_color(); break;
            case 0x20:
                for (int v = 0; v < 3; ++v) {
                    light(v);
                    push_color();
                }
                break;
            case 0x28: sqr(); break;
            case 0x29: color_interp(); break;
            case 0x2A:
                for (int k = 0; k < 3; ++k) dpc(d(20));
                break;
            case 0x2D: avsz(false); break;
            case 0x2E: avsz(true); break;
            case 0x30:
                for (int v = 0; v < 3; ++v) rtp(v, v == 2);
                break;
            case 0x3D: gpf(false); break;
            case 0x3E: gpf(true); break;
            case 0x3F:
                for (int v = 0; v < 3; ++v) {
                    light(v);
                    color_mult();
                }
                break;
            default: break;
        }
        if (flag & flag_error_mask) flag |= 0x80000000u;
        g.ctrl_[31] = flag;
    }
};

uint32_t Gte::read_data(unsigned reg) const {
    reg &= 31;
    switch (reg) {
        case 1: case 3: case 5:  // VZn
            return static_cast<uint32_t>(static_cast<int32_t>(s16(data_[reg])));
        case 7:  // OTZ
        case 16: case 17: case 18: case 19:  // SZn
            return data_[reg] & 0xFFFFu;
        case 8: case 9: case 10: case 11:  // IR0-3 (stored sign-extended)
            return data_[reg];
        case 15:  // SXYP reads SXY2
            return data_[14];
        case 28:
        case 29: {  // IRGB / ORGB
            uint32_t v = 0;
            for (int i = 0; i < 3; ++i) {
                int32_t c = static_cast<int32_t>(data_[9 + i]) >> 7;
                v |= static_cast<uint32_t>(std::clamp(c, 0, 0x1F)) << (5 * i);
            }
            return v;
        }
        case 31: {  // LZCR
            int32_t s = static_cast<int32_t>(data_[30]);
            uint32_t u = static_cast<uint32_t>(s < 0 ? ~s : s);
            return static_cast<uint32_t>(std::countl_zero(u));
        }
        default: return data_[reg];
    }
}

void Gte::write_data(unsigned reg, uint32_t value) {
    reg &= 31;
    switch (reg) {
        case 1: case 3: case 5:
            data_[reg] = static_cast<uint32_t>(static_cast<int32_t>(s16(value)));
            break;
        case 7:
        case 16: case 17: case 18: case 19:
            data_[reg] = value & 0xFFFFu;
            break;
        case 8: case 9: case 10: case 11:
            data_[reg] = static_cast<uint32_t>(static_cast<int32_t>(s16(value)));
            break;
        case 15:  // SXYP push
            data_[12] = data_[13];
            data_[13] = data_[14];
            data_[14] = value;
            break;
        case 28:  // IRGB
            data_[9] = (value & 0x1F) * 0x80;
            data_[10] = ((value >> 5) & 0x1F) * 0x80;
            data_[11] = ((value >> 10) & 0x1F) * 0x80;
            break;
        case 29:  // ORGB is read-only
        case 31:  // LZCR is read-only
            break;
        default: data_[reg] = value; break;
    }
}

uint32_t Gte::read_ctrl(unsigned reg) const {
    reg &= 31;
    switch (reg) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return static_cast<uint32_t>(static_cast<int32_t>(s16(ctrl_[reg])));
        case 31: {
            uint32_t f = ctrl_[31] & 0x7FFFF000u;
            return f | ((f & flag_error_mask) ? 0x80000000u : 0u);
        }
        default: return ctrl_[reg];
    }
}

void Gte::write_ctrl(unsigned reg, uint32_t value) {
    reg &= 31;
    switch (reg) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            ctrl_[reg] = static_cast<uint32_t>(static_cast<int32_t>(s16(value)));
            break;
        case 31: ctrl_[31] = value & 0x7FFFF000u; break;
        default: ctrl_[reg] = value; break;
    }
}

void Gte::execute(uint32_t command) {
    cmd_ = command;
    GteImpl impl(*this);
    impl.run(command);
}

int Gte::command_cycles(uint32_t command) {
    switch (cmd_op(command)) {
        case 0x01: return 15;
        case 0x06: return 8;
        case 0x0C: return 6;
        case 0x10: return 8;
        case 0x11: return 8;
        case 0x12: return 8;
        case 0x13: return 19;
        case 0x14: return 13;
        case 0x16: return 44;
        case 0x1B: return 17;
        case 0x1C: return 11;
        case 0x1E: return 14;
        case 0x20: return 30;
        case 0x28: return 5;
        case 0x29: return 8;
        case 0x2A: return 17;
        case 0x2D: return 5;
        case 0x2E: return 6;
        case 0x30: return 23;
        case 0x3D: return 5;
        case 0x3E: return 5;
        case 0x3F: return 39;
        default: return 1;
    }
}

void Gte::reset() { *this = Gte{}; }

void Gte::io(StateIo& s) {
    StateIo::Section sec(s, "GTE0");
    for (auto& r : data_) s.io(r);
    for (auto& r : ctrl_) s.io(r);
    s.io(cmd_);
}

}  // namespace dw3
