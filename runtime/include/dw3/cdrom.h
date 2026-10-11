#pragma once

// CD-ROM controller at 0x1F801800-0x1F801803 (index-multiplexed registers, parameter/response/data FIFOs,
// command set, sector streaming, XA routing, DMA channel 3).
//
// Behaviour follows the psx-spx CD-ROM Drive documentation. Command semantics, the XA routing rule
// ((submode & 0x44) == 0x44 with Mode.XA) and the INT1-drop-on-overrun policy were cross-checked against
// the approach of PCSX-Redux (GPL-2+) and Beetle PSX / Mednafen (GPL-2+); no code was copied.
//
// The disc is reached only through SectorSource so the runtime does not depend on the disc library.

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"

namespace dw3 {

// Raw 2352-byte sector access. LBA 0 is MSF 00:02:00 (so absolute MSF frames = lba + 150).
struct SectorSource {
    virtual ~SectorSource() = default;
    virtual uint32_t sector_count() const = 0;
    virtual bool read_raw(uint32_t lba, uint8_t out[2352]) = 0;
};

// Receives XA-ADPCM sectors (raw 2352 bytes, valid only during the call) that passed the Mode/filter rules.
struct XaSink {
    virtual ~XaSink() = default;
    virtual void xa_sector(const uint8_t* raw2352) = 0;
};

class Cdrom : public DmaDevice {
public:
    static constexpr uint32_t kBase = 0x1F801800u;
    static constexpr uint32_t kRawSize = 2352;

    // Timing (CPU cycles at 33.8688 MHz).
    static constexpr int64_t kCyclesPerSector1x = 451584;  // 75 sectors/s
    static constexpr int64_t kCyclesPerSector2x = 225792;
    static constexpr int64_t kFirstResponse = 4000;        // command write -> first response
    static constexpr int64_t kAckGap = 1000;               // ack of INT -> next queued response
    static constexpr int64_t kInitDelay = 80000;
    static constexpr int64_t kGetIdDelay = 20000;
    static constexpr int64_t kPauseIdle = 7000;            // Pause when not reading
    static constexpr int64_t kPause1x = 2100000;
    static constexpr int64_t kPause2x = 1100000;
    static constexpr int64_t kStopDelay = 5000000;
    static constexpr int64_t kMotorOnDelay = 4000000;
    static constexpr int64_t kSpinUp = 1000000;            // extra delay to first sector with motor off
    static constexpr int64_t kSeekBase = 200000;
    static constexpr int64_t kSeekPerSector = 24;          // capped at kSeekMaxSectors
    static constexpr int64_t kSeekMaxSectors = 250000;
    static constexpr int64_t kReadTocDelay = 4000000;
    static constexpr int64_t kDmaCyclesPerWord = 24;

    // Stat byte bits.
    enum : uint8_t {
        kStatError = 0x01, kStatMotor = 0x02, kStatSeekError = 0x04, kStatIdError = 0x08,
        kStatShellOpen = 0x10, kStatRead = 0x20, kStatSeek = 0x40, kStatPlay = 0x80,
    };
    // Setmode bits.
    enum : uint8_t {
        kModeCdda = 0x01, kModeAutoPause = 0x02, kModeReport = 0x04, kModeFilter = 0x08,
        kModeIgnore = 0x10, kModeWholeSector = 0x20, kModeXa = 0x40, kModeDouble = 0x80,
    };
    // Error codes (second response byte of INT5).
    enum : uint8_t {
        kErrSeek = 0x04, kErrBadParam = 0x10, kErrBadParamCount = 0x20, kErrBadCommand = 0x40, kErrNotReady = 0x80,
    };
    // 0x1F801800 status register bits.
    enum : uint8_t {
        kStsAdpBusy = 0x04, kStsParamEmpty = 0x08, kStsParamReady = 0x10, kStsRespReady = 0x20,
        kStsDataReq = 0x40, kStsBusy = 0x80,
    };

    enum class State : uint8_t { Idle, Seeking, Reading, Playing };

    struct Volume {
        uint8_t ll = 0x80, lr = 0, rl = 0, rr = 0x80;  // CD left/right -> SPU left/right
    };

    Cdrom(Bus& bus, Scheduler& sched, IrqController& irq, Dma& dma);

    void set_disc(SectorSource* disc);  // disc present and spinning after this (null: no disc, motor off)
    void set_xa_sink(XaSink* sink) { xa_sink_ = sink; }
    void set_shell_open(bool open) { shell_open_ = open; }
    void reset();  // power-on state (keeps disc/sink)

    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t value, int size);

    // DmaDevice (channel 3): drains the data FIFO.
    void dma_read(std::span<uint32_t> out) override;
    uint32_t dma_cycles_per_word() const override { return kDmaCyclesPerWord; }

    // Introspection (tests, audio mixer).
    uint8_t stat() const;
    uint8_t mode() const { return mode_; }
    State state() const { return state_; }
    bool motor_on() const { return motor_on_; }
    int64_t next_lba() const { return lba_; }
    uint8_t irq_flag() const { return flag_; }
    uint8_t irq_enable() const { return enable_; }
    const Volume& volume() const { return volume_; }
    bool adpcm_muted() const { return adpcm_mute_; }
    bool cdda_muted() const { return muted_; }
    uint64_t overruns() const { return overruns_; }
    std::size_t queued_responses() const { return queue_.size(); }
    std::size_t data_fifo_remaining() const { return data_len_ - data_pos_; }

private:
    struct Resp {
        uint8_t int_no;
        std::vector<uint8_t> data;
        bool first;  // first response of a command: clears BUSYSTS when delivered
    };

    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);
    uint8_t status_reg() const;
    uint8_t data_byte();
    void load_data_fifo();
    void write_flag(uint8_t v);

    void start_command(uint8_t cmd);
    void execute(uint8_t cmd, const std::vector<uint8_t>& p);
    void enqueue(uint8_t int_no, std::vector<uint8_t> data, bool first = false);
    void deliver(Resp r);
    void schedule_flush();
    void update_line();
    void later(int64_t delay, std::function<void()> fn);

    void start_stream(bool play);
    void stop_stream();
    void sector_tick(int64_t due);
    bool route_xa(const uint8_t* raw);
    int64_t period() const { return (mode_ & kModeDouble) ? kCyclesPerSector2x : kCyclesPerSector1x; }
    static int64_t seek_cycles(int64_t from, int64_t to);
    int64_t sector_total() const;

    Scheduler& sched_;
    IrqController& irq_;
    Dma& dma_;
    SectorSource* disc_ = nullptr;
    XaSink* xa_sink_ = nullptr;

    // Registers.
    uint8_t index_ = 0, enable_ = 0, flag_ = 0;
    bool line_ = false;
    bool busy_ = false;
    uint8_t request_ = 0;
    std::vector<uint8_t> params_;
    std::array<uint8_t, 16> resp_{};
    uint8_t resp_len_ = 0, resp_pos_ = 0;
    std::deque<Resp> queue_;
    EventId flush_ev_ = kNoEvent;
    uint64_t epoch_ = 0;

    // Data FIFO.
    std::array<uint8_t, 0x930> data_{};
    uint32_t data_len_ = 0, data_pos_ = 0;
    std::array<uint8_t, kRawSize> ready_{};
    bool have_ready_ = false;

    // Audio.
    Volume volume_{}, pending_volume_{};
    bool adpcm_mute_ = false, muted_ = false;
    uint8_t sound_map_ = 0, sound_coding_ = 0;

    // Drive state.
    State state_ = State::Idle;
    bool stream_play_ = false;
    bool motor_on_ = false, shell_open_ = false;
    uint8_t mode_ = 0, filter_file_ = 0, filter_chan_ = 0;
    int64_t lba_ = 0;       // next sector to read
    int64_t last_lba_ = 0;  // head position (last sector read / seek target)
    bool setloc_pending_ = false;
    int64_t setloc_lba_ = 0;
    bool have_last_ = false;
    std::array<uint8_t, 8> last_hdr_{};  // header(4) + subheader(4) of the last sector processed
    EventId sector_ev_ = kNoEvent;
    std::array<uint8_t, kRawSize> raw_{};
    uint64_t overruns_ = 0;
};

}  // namespace dw3
