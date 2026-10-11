// MDEC model. Register/command/decoding rules from psx-spx "Macroblock Decoder (MDEC)"
// (https://psx-spx.consoledev.net/macroblockdecodermdec/). Algorithm structure cross-checked against the
// published behaviour of PCSX-Redux / Mednafen (GPL-2+); no code copied.

#include "dw3/mdec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dw3 {

namespace {

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

inline int32_t s10(uint32_t v) { return static_cast<int32_t>((v & 0x3FFu) << 22) >> 22; }
inline int32_t clamp8(int32_t v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }

}  // namespace

std::array<int16_t, 64> Mdec::default_scale_table() {
    std::array<int16_t, 64> t{};
    const double pi = std::acos(-1.0);
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x)
            t[u * 8 + x] = u == 0 ? int16_t{0x5A82}
                                  : static_cast<int16_t>(std::lround(32768.0 * std::cos((2 * x + 1) * u * pi / 16.0)));
    return t;
}

Mdec::Mdec(Bus& bus, Scheduler& sched, Dma& dma) : sched_(sched), dma_(dma) {
    out_buf_.assign(1u << 16, 0);
    auto t = default_scale_table();
    for (int i = 0; i < 64; ++i) scale_[i] = t[i] >> 3;
    bus.map_io({kBase, kSize, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
    dma.attach(DmaChannel::MdecIn, &in_);
    dma.attach(DmaChannel::MdecOut, &out_);
    reset();
}

// ---- registers ------------------------------------------------------------------------------------------

uint32_t Mdec::read(uint32_t off, int size) {
    uint32_t shift = (off & 3u) * 8;
    uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
    if (!(off & 4u)) {
        // Data port: peek the head word; pop once its last byte has been read.
        uint32_t w = available_words() ? out_buf_[head_] : 0;
        if (size >= 4 || shift + size * 8 >= 32) read_data();
        return (w >> shift) & mask;
    }
    return (status() >> shift) & mask;
}

void Mdec::write(uint32_t off, uint32_t v, int size) {
    uint32_t shift = (off & 3u) * 8;
    if (size >= 4) {
        if (off & 4u) write_control(v);
        else write_word(v);
        return;
    }
    uint32_t mask = ((1u << (size * 8)) - 1) << shift;
    stage_word_ = (stage_word_ & ~mask) | ((v << shift) & mask);
    if (shift + size * 8 >= 32) {
        uint32_t w = stage_word_;
        stage_word_ = 0;
        if (off & 4u) write_control(w);
        else write_word(w);
    }
}

uint32_t Mdec::status() const {
    uint32_t s = remaining_status_ & 0xFFFFu;
    s |= (cur_block_ & 7u) << 16;
    s |= (bit15_ ? 1u : 0u) << 23;
    s |= (signed_ ? 1u : 0u) << 24;
    s |= (depth_ & 3u) << 25;
    if (out_enable_ && available_words()) s |= 1u << 27;
    if (in_enable_) s |= 1u << 28;
    if (decoding() || state_ != State::Idle) s |= 1u << 29;
    if (!available_words()) s |= 1u << 31;
    return s;
}

bool Mdec::decoding() const { return pushed_ != popped_ && sched_.now() < last_at_; }

uint32_t Mdec::available_words() const {
    int64_t now = sched_.now();
    while (chunk_ready_ < chunks_.size() && chunks_[chunk_ready_].at <= now) ready_end_ = chunks_[chunk_ready_++].end;
    return static_cast<uint32_t>(ready_end_ - popped_);
}

uint32_t Mdec::read_data() {
    if (!available_words()) return 0;
    uint32_t w = out_buf_[head_++];
    ++popped_;
    if (head_ == tail_) drained();
    return w;
}

void Mdec::reset() {
    if (ev_ != kNoEvent) sched_.cancel(ev_);
    ev_ = kNoEvent;
    state_ = State::Idle;
    stage_ = Stage::First;
    params_left_ = table_pos_ = 0;
    blk_ = n_ = 0;
    head_ = tail_ = 0;
    chunks_.clear();
    chunk_ready_ = 0;
    ready_end_ = pushed_ = popped_ = 0;
    last_at_ = 0;
    depth_ = 0;
    signed_ = bit15_ = false;
    cur_block_ = 4;
    remaining_status_ = 0;
    stage_word_ = 0;
}

void Mdec::write_control(uint32_t v) {
    if (v & 0x80000000u) reset();
    in_enable_ = (v >> 30) & 1u;
    out_enable_ = (v >> 29) & 1u;
    if (in_enable_) dma_.device_ready(DmaChannel::MdecIn);
    if (out_enable_ && available_words()) dma_.device_ready(DmaChannel::MdecOut);
}

// ---- commands -------------------------------------------------------------------------------------------

void Mdec::write_word(uint32_t w) {
    switch (state_) {
        case State::Idle: start_command(w); return;
        case State::Quant: {
            auto& tbl = table_pos_ < 16 ? qt_luma_ : qt_chroma_;
            uint32_t base = (table_pos_ & 15u) * 4;
            for (int i = 0; i < 4; ++i) tbl[base + i] = static_cast<uint8_t>(w >> (8 * i));
            break;
        }
        case State::Scale: {
            uint32_t base = table_pos_ * 2;
            scale_[base] = static_cast<int16_t>(w) >> 3;
            scale_[base + 1] = static_cast<int16_t>(w >> 16) >> 3;
            break;
        }
        case State::Decode:
            feed_halfword(static_cast<uint16_t>(w));
            feed_halfword(static_cast<uint16_t>(w >> 16));
            break;
    }
    ++table_pos_;
    if (--params_left_ == 0) {
        state_ = State::Idle;
        remaining_status_ = 0xFFFFu;
        stage_ = Stage::First;
        cur_block_ = 4;
    } else {
        remaining_status_ = (params_left_ - 1) & 0xFFFFu;
    }
}

void Mdec::start_command(uint32_t w) {
    // Output format bits are latched from every command word.
    depth_ = (w >> 27) & 3u;
    signed_ = (w >> 26) & 1u;
    bit15_ = (w >> 25) & 1u;
    table_pos_ = 0;
    stage_ = Stage::First;
    blk_ = 0;
    switch (w >> 29) {
        case 1:
            params_left_ = w & 0xFFFFu;
            state_ = params_left_ ? State::Decode : State::Idle;
            break;
        case 2:
            quant_both_ = w & 1u;
            params_left_ = quant_both_ ? 32 : 16;
            state_ = State::Quant;
            break;
        case 3:
            params_left_ = 32;
            state_ = State::Scale;
            break;
        default:  // 0, 4-7: no-ops
            params_left_ = 0;
            state_ = State::Idle;
            break;
    }
    remaining_status_ = state_ == State::Idle ? 0xFFFFu : ((params_left_ - 1) & 0xFFFFu);
    cur_block_ = state_ == State::Decode ? (depth_ >= 2 ? 4u : 0u) : 4u;
}

// ---- block decoding -------------------------------------------------------------------------------------

void Mdec::store_coef(int n, int32_t val, uint32_t qscale, int32_t raw) {
    if (qscale == 0) val = raw * 2;
    val = std::clamp(val, -0x400, 0x3FF);
    coef_[qscale ? kZigzag[n] : n] = val;
}

void Mdec::feed_halfword(uint16_t h) {
    bool colour = depth_ >= 2;
    if (stage_ == Stage::First) {
        if (h == 0xFE00) return;  // padding before a block
        const auto& qt = (colour && blk_ < 2) ? qt_chroma_ : qt_luma_;
        coef_.fill(0);
        qscale_ = h >> 10;
        n_ = 0;
        int32_t raw = s10(h);
        store_coef(0, raw * qt[0], qscale_, raw);
        stage_ = Stage::Run;
        return;
    }
    n_ += ((h >> 10) & 0x3F) + 1;
    if (n_ > 63) {
        finish_block();
        return;
    }
    const auto& qt = (colour && blk_ < 2) ? qt_chroma_ : qt_luma_;
    int32_t raw = s10(h);
    store_coef(n_, (raw * qt[n_] * static_cast<int32_t>(qscale_) + 4) >> 3, qscale_, raw);
}

void Mdec::idct(const int32_t* src, int32_t* dst) const {
    int32_t tmp[64];
    const int32_t* sc = scale_.data();
    for (int pass = 0; pass < 2; ++pass) {
        int32_t* d = pass == 0 ? tmp : dst;
        const int32_t* s = pass == 0 ? src : tmp;
        for (int x = 0; x < 8; ++x)
            for (int y = 0; y < 8; ++y) {
                int64_t sum = 0;
                for (int z = 0; z < 8; ++z) sum += static_cast<int64_t>(s[y + z * 8]) * sc[x + z * 8];
                d[x + y * 8] = static_cast<int32_t>((sum + 0xFFF) >> 13);
            }
    }
}

void Mdec::finish_block() {
    stage_ = Stage::First;
    if (depth_ < 2) {  // mono: one Y block per macroblock
        idct(coef_.data(), y_[0].data());
        emit_mono();
        mb_count_++;
        return;
    }
    int32_t* dst = blk_ == 0 ? cr_.data() : blk_ == 1 ? cb_.data() : y_[blk_ - 2].data();
    idct(coef_.data(), dst);
    if (++blk_ == 6) {
        blk_ = 0;
        emit_macroblock();
        mb_count_++;
    }
    cur_block_ = blk_ == 0 ? 4u : blk_ == 1 ? 5u : static_cast<uint32_t>(blk_ - 2);
}

uint32_t* Mdec::reserve_out(uint32_t words) {
    if (tail_ + words > out_buf_.size()) {
        if (head_ > 0) {
            std::memmove(out_buf_.data(), out_buf_.data() + head_, (tail_ - head_) * 4);
            tail_ -= head_;
            head_ = 0;
        }
        if (tail_ + words > out_buf_.size()) out_buf_.resize(std::max<size_t>(out_buf_.size() * 2, tail_ + words), 0);
    }
    uint32_t* p = out_buf_.data() + tail_;
    tail_ += words;
    return p;
}

void Mdec::drained() {
    head_ = tail_ = 0;
    chunks_.clear();
    chunk_ready_ = 0;
}

void Mdec::note_output(uint32_t words, int64_t blocks) {
    last_at_ = std::max(last_at_, sched_.now()) + blocks * kCyclesPerBlock;
    pushed_ += words;
    chunks_.push_back({pushed_, last_at_});
    if (ev_ == kNoEvent) schedule_next();
}

// Wake at the next macroblock that is not yet visible.
void Mdec::schedule_next() {
    available_words();  // advance chunk_ready_
    if (chunk_ready_ >= chunks_.size()) return;
    ev_ = sched_.schedule(chunks_[chunk_ready_].at, [this](int64_t) { on_ready(); });
}

void Mdec::on_ready() {
    ev_ = kNoEvent;
    if (out_enable_ && available_words()) dma_.device_ready(DmaChannel::MdecOut);
    if (ev_ == kNoEvent) schedule_next();
}

// Words the pending DMA1 transfer wants (from its BCR); 1 if unknown.
uint32_t Mdec::dma_out_words() const {
    uint32_t bcr = dma_.bcr(DmaChannel::MdecOut), sync = (dma_.chcr(DmaChannel::MdecOut) >> 9) & 3u;
    uint32_t bs = bcr & 0xFFFFu ? bcr & 0xFFFFu : 0x10000u;
    return sync == 1 ? bs * (bcr >> 16) : bs;
}

void Mdec::emit_mono() {
    uint8_t px[64];
    const uint32_t flip = signed_ ? 0u : 0x80u;
    for (int i = 0; i < 64; ++i) px[i] = static_cast<uint8_t>((static_cast<uint32_t>(clamp8(y_[0][i])) & 0xFFu) ^ flip);
    if (depth_ == k8Bit) {
        std::memcpy(reserve_out(16), px, 64);
    } else {  // 4-bit: high nibble of Y, first pixel in the low nibble
        uint8_t packed[32];
        for (int i = 0; i < 32; ++i) packed[i] = static_cast<uint8_t>((px[2 * i] >> 4) | (px[2 * i + 1] & 0xF0));
        std::memcpy(reserve_out(8), packed, 32);
    }
    note_output(depth_ == k8Bit ? 16 : 8, 1);
}

void Mdec::emit_macroblock() {
    const uint32_t flip = signed_ ? 0u : 0x80u;
    const bool is24 = depth_ == k24Bit;
    uint8_t* dst = reinterpret_cast<uint8_t*>(reserve_out(is24 ? 192 : 128));
    const uint16_t hi = bit15_ ? 0x8000u : 0u;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            int ci = (y >> 1) * 8 + (x >> 1);
            int32_t cr = cr_[ci], cb = cb_[ci];
            int32_t yy = y_[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)];
            int32_t r = clamp8(yy + ((91881 * cr + 32768) >> 16));
            int32_t g = clamp8(yy + ((-22525 * cb - 46812 * cr + 32768) >> 16));
            int32_t b = clamp8(yy + ((116130 * cb + 32768) >> 16));
            uint32_t ur = (static_cast<uint32_t>(r) & 0xFFu) ^ flip, ug = (static_cast<uint32_t>(g) & 0xFFu) ^ flip,
                     ub = (static_cast<uint32_t>(b) & 0xFFu) ^ flip;
            if (is24) {
                *dst++ = static_cast<uint8_t>(ur);
                *dst++ = static_cast<uint8_t>(ug);
                *dst++ = static_cast<uint8_t>(ub);
            } else {
                uint16_t p = static_cast<uint16_t>((ur >> 3) | ((ug >> 3) << 5) | ((ub >> 3) << 10)) | hi;
                *dst++ = static_cast<uint8_t>(p);
                *dst++ = static_cast<uint8_t>(p >> 8);
            }
        }
    }
    note_output(is24 ? 192 : 128, 6);
}

// ---- DMA adapters ---------------------------------------------------------------------------------------

bool Mdec::In::dma_ready(bool to_ram) const { return to_ram ? false : m_.in_enable_; }
void Mdec::In::dma_write(std::span<const uint32_t> words) {
    for (uint32_t w : words) m_.write_word(w);
}
void Mdec::In::dma_read(std::span<uint32_t> out) { std::fill(out.begin(), out.end(), 0u); }

bool Mdec::Out::dma_ready(bool to_ram) const { return to_ram && m_.out_enable_ && m_.available_words() >= std::max(1u, m_.dma_out_words()); }
void Mdec::Out::dma_write(std::span<const uint32_t>) {}
void Mdec::Out::dma_read(std::span<uint32_t> out) {
    uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(out.size()), m_.available_words());
    if (n) std::memcpy(out.data(), m_.out_buf_.data() + m_.head_, n * 4);
    m_.head_ += n;
    m_.popped_ += n;
    if (m_.head_ == m_.tail_) m_.drained();
    std::fill(out.begin() + n, out.end(), 0u);
}

}  // namespace dw3
