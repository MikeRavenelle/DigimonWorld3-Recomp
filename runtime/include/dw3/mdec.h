#pragma once

// PS1 MDEC (motion decoder) at 0x1F801820/0x1F801824, DMA channel 0 (MDECin) and 1 (MDECout).
// Register, command, RLE/dequantisation and IDCT behaviour follow psx-spx "Macroblock Decoder (MDEC)".
// The IDCT is two 1D passes through the uploaded scale table (entries >> 3, +0xFFF, >> 13). YUV->RGB uses
// 16.16 fixed point with the JPEG coefficients (1.402, 1.772, -0.3437, -0.7143), clamped to -128..127.
//
// Model: command words are consumed as they arrive (CPU writes or DMA0); every completed macroblock is
// decoded immediately into an output word queue, which becomes visible to the CPU/DMA1 after a modelled
// decode delay (Scheduler event). No allocation happens per block.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/scheduler.h"
#include "dw3/state.h"

namespace dw3 {

class Mdec {
public:
    static constexpr uint32_t kBase = 0x1F801820u;
    static constexpr uint32_t kSize = 8;
    static constexpr int64_t kCyclesPerBlock = 250;  // modelled cost of one 8x8 block (decode + output)

    enum Depth : uint32_t { k4Bit = 0, k8Bit = 1, k24Bit = 2, k15Bit = 3 };

    Mdec(Bus& bus, Scheduler& sched, Dma& dma);
    Mdec(const Mdec&) = delete;
    Mdec& operator=(const Mdec&) = delete;

    // Register-level access (offset 0 = command/data, 4 = control/status), any access size.
    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t value, int size);

    void write_word(uint32_t word);  // command or parameter word
    void write_control(uint32_t v);
    uint32_t status() const;
    uint32_t read_data();  // pops one decoded word (0 when none is available)

    // Words the CPU/DMA1 can fetch right now (decoded and past the modelled delay).
    uint32_t available_words() const;  // decoded macroblocks whose modelled delay has elapsed
    uint32_t queued_words() const { return tail_ - head_; }
    bool decoding() const;

    const std::array<uint8_t, 64>& luma_table() const { return qt_luma_; }
    const std::array<uint8_t, 64>& chroma_table() const { return qt_chroma_; }
    const std::array<int32_t, 64>& scale_table() const { return scale_; }  // stored already >> 3
    uint64_t macroblocks_decoded() const { return mb_count_; }

    // Save state: control/command state, tables, the block decoder, queued output with its ready times and the
    // pending wake-up event.
    void io(StateIo& s);

    // Standard IDCT matrix (what libpress uploads): 0x5A82 for row 0, 32768*cos((2x+1)*u*pi/16) otherwise.
    static std::array<int16_t, 64> default_scale_table();

private:
    enum class State { Idle, Decode, Quant, Scale };
    enum class Stage { First, Run };

    struct In : DmaDevice {
        explicit In(Mdec& m) : m_(m) {}
        bool dma_ready(bool to_ram) const override;
        void dma_write(std::span<const uint32_t> words) override;
        void dma_read(std::span<uint32_t> out) override;
        Mdec& m_;
    };
    struct Out : DmaDevice {
        explicit Out(Mdec& m) : m_(m) {}
        bool dma_ready(bool to_ram) const override;
        void dma_write(std::span<const uint32_t> words) override;
        void dma_read(std::span<uint32_t> out) override;
        Mdec& m_;
    };

    void reset();
    void start_command(uint32_t w);
    void feed_halfword(uint16_t h);
    void store_coef(int n, int32_t val, uint32_t qscale, int32_t raw);
    void finish_block();
    void idct(const int32_t* src, int32_t* dst) const;
    void emit_macroblock();
    void emit_mono();
    uint32_t* reserve_out(uint32_t words);
    void note_output(uint32_t words, int64_t blocks);
    uint32_t dma_out_words() const;
    void schedule_next();
    void drained();
    void on_ready();

    Scheduler& sched_;
    Dma& dma_;
    In in_{*this};
    Out out_{*this};

    // Control / status.
    bool in_enable_ = false, out_enable_ = false;
    uint32_t depth_ = 0;
    bool signed_ = false, bit15_ = false;
    uint32_t cur_block_ = 4;
    uint32_t remaining_status_ = 0;  // bits 0-15 of the status register

    // Command state.
    State state_ = State::Idle;
    uint32_t params_left_ = 0;
    uint32_t table_pos_ = 0;  // words received for quant/scale upload
    bool quant_both_ = false;
    std::array<uint8_t, 64> qt_luma_{}, qt_chroma_{};
    std::array<int32_t, 64> scale_{};
    uint32_t stage_word_ = 0;  // sub-word write assembly (data port)

    // Block decoder.
    Stage stage_ = Stage::First;
    int blk_ = 0;  // index in decode order: colour 0=Cr 1=Cb 2..5=Y1..Y4; mono 0
    int n_ = 0;
    uint32_t qscale_ = 0;
    std::array<int32_t, 64> coef_{};
    std::array<int32_t, 64> cr_{}, cb_{};
    std::array<std::array<int32_t, 64>, 4> y_{};
    uint64_t mb_count_ = 0;

    // Output queue (word ring-ish buffer: head_/tail_ indexes into out_).
    std::vector<uint32_t> out_buf_;
    uint32_t head_ = 0, tail_ = 0;
    // Visibility: each macroblock's words become fetchable at its own time. Absolute word counters.
    struct Chunk {
        uint64_t end;  // absolute index one past the chunk's last word
        int64_t at;    // cycle at which it becomes visible
    };
    std::vector<Chunk> chunks_;
    mutable size_t chunk_ready_ = 0;  // chunks_[0..chunk_ready_) are visible
    mutable uint64_t ready_end_ = 0;
    uint64_t pushed_ = 0, popped_ = 0;
    int64_t last_at_ = 0;
    EventId ev_ = kNoEvent;
};

}  // namespace dw3
