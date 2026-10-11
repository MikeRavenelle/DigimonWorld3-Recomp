#pragma once

// SPU signal-processing building blocks (pure functions / small state machines, unit-tested in isolation).
// Behaviour follows psx-spx "Sound Processing Unit (SPU)" and "CDROM XA Audio ADPCM Compression"
// (https://psx-spx.consoledev.net/). Where psx-spx is silent, conventions match PCSX-Redux and Beetle PSX /
// Mednafen (both GPL-2.0-or-later): key-on resets the repeat address, shift > 12 acts as 9, filters 5-7 act as 4.

#include <array>
#include <cstdint>
#include <span>

#include "dw3/state.h"

namespace dw3::spu_dsp {

inline constexpr std::array<int16_t, 512> kGauss = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963};

// 25-point zigzag resampler tables for 37.8 kHz -> 44.1 kHz XA output: kZigzag[phase][i - 1] for taps i = 1..29.
inline constexpr std::array<std::array<int16_t, 29>, 7> kZigzag = {{
    {
        0, 0, 0, 0, 0, -2, 10, -34, 65, -84, 52, 9, -266, 1024,
        -2680, 9036, 26516, -6016, 3021, -1571, 848, -365, 107, 10, -16, 17, -8, 3,
        -1},
    {
        0, 0, 0, -2, 0, 3, -19, 60, -75, 162, -227, 306, -67, -615,
        3229, 29883, -4532, 2488, -1471, 882, -424, 166, -27, 5, 6, -8, 3, -1,
        0},
    {
        0, 0, -1, 3, -2, -5, 31, -74, 179, -402, 689, -926, 1272, -1446,
        31033, -1446, 1272, -926, 689, -402, 179, -74, 31, -5, -2, 3, -1, 0,
        0},
    {
        0, -1, 3, -8, 6, 5, -27, 166, -424, 882, -1471, 2488, -4532, 29883,
        3229, -615, -67, 306, -227, 162, -75, 60, -19, 3, 0, -2, 0, 0,
        0},
    {
        -1, 3, -8, 17, -16, 10, 107, -365, 848, -1571, 3021, -6016, 26516, 9036,
        -2680, 1024, -266, 9, 52, -84, 65, -34, 10, -1, 0, 1, 0, 0,
        0},
    {
        2, -8, 16, -35, 43, 26, -235, 635, -1352, 2810, -5882, 21472, 15367, -4681,
        2062, -839, 347, -68, -23, 70, -35, 17, -5, 0, 0, 0, 0, 0,
        0},
    {
        -5, 17, -35, 70, -23, -68, 347, -839, 2062, -4681, 15367, 21472, -5882, 2810,
        -1352, 635, -235, 26, 43, -35, 16, -8, 2, 0, 0, 0, 0, 0,
        0},
}};

inline constexpr std::array<int8_t, 5> kAdpcmPos = {0, 60, 115, 98, 122};
inline constexpr std::array<int8_t, 5> kAdpcmNeg = {0, 0, -52, -55, -60};

inline int16_t clamp16(int32_t v) { return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

// Decodes one 16-byte SPU-ADPCM block into 28 samples. s1/s2 are the previous two output samples (updated).
void decode_adpcm_block(const uint8_t* block16, int16_t* out28, int32_t& s1, int32_t& s2);
// Flag byte (block[1]) bits.
inline constexpr uint8_t kFlagLoopEnd = 1, kFlagLoopRepeat = 2, kFlagLoopStart = 4;

// 4-point Gaussian interpolation; w[0] = oldest ... w[3] = newest; idx = pitch counter bits 4..11.
int16_t gauss_interpolate(const int16_t* w, unsigned idx);

// ---- ADSR / sweep envelope (psx-spx "Envelope Operation depending on Shift/Step/Mode/Direction") ----
struct EnvRate {
    uint8_t shift = 0;           // 0..31
    uint8_t step = 0;            // 0..3 (+7,+6,+5,+4 or -8,-7,-6,-5)
    bool decreasing = false;
    bool exponential = false;
    bool phase_negative = false;  // sweep phase bit
    bool never_steps = false;     // all rate bits set: the counter never reaches a step and never saturates
};
// Advances one 44.1 kHz tick. level is the volume (-0x8000..0x7FFF), counter the rate counter.
void envelope_tick(int32_t& level, int32_t& counter, const EnvRate& r);

// Noise generator (shared by all voices). Levels are signed 16-bit.
struct Noise {
    uint16_t level = 0;
    int32_t timer = 0;
    void tick(unsigned shift, unsigned step);  // shift 0..15 (SPUCNT bits 13-10), step 0..3 (bits 9-8) = 4..7
    void io(StateIo& s);
};

// Reverb unit: 22.05 kHz core fed/drained through 39-tap resamplers. Registers are the 32 words at 0x1DC0..0x1DFF.
class Reverb {
public:
    enum Reg : unsigned {
        dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
        mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1,
        mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN
    };
    static constexpr std::array<int16_t, 39> kFir = {
        -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0,
        1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};

    // Called by the SPU when ESA is written: also moves the current buffer address there.
    void set_start(uint32_t esa_bytes) {
        start_ = esa_bytes;
        cur_ = esa_bytes;
    }
    uint32_t start() const { return start_; }
    uint32_t current() const { return cur_; }
    void io(StateIo& s);

    // One 44.1 kHz tick: in_l/in_r is the reverb input mix; returns the upsampled reverb output (before EVOL).
    // write = SPUCNT bit 7 (buffer writes + same/diff stages enabled).
    void tick(uint8_t* ram, const uint16_t* regs, bool write, int16_t in_l, int16_t in_r, int16_t& out_l,
              int16_t& out_r);

private:
    int16_t rd(const uint8_t* ram, int32_t off) const;
    void wr(uint8_t* ram, int32_t off, int16_t v) const;
    void compute(uint8_t* ram, const uint16_t* regs, bool write, int16_t xl, int16_t xr, int16_t& ol, int16_t& or_);

    uint32_t start_ = 0, cur_ = 0;
    unsigned phase_ = 0;
    std::array<std::array<int16_t, 64>, 2> in_hist_{};
    std::array<std::array<int16_t, 32>, 2> out_hist_{};
    unsigned in_pos_ = 0, out_pos_ = 0;
};

}  // namespace dw3::spu_dsp
