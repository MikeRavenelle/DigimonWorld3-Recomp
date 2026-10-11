#pragma once

// SIO0: controller / memory card serial port at 0x1F801040-0x1F80104F (JOY_DATA, JOY_STAT, JOY_MODE,
// JOY_CTRL, JOY_BAUD), with digital pads and 128 KiB memory cards on two ports.
//
// Behaviour follows the psx-spx "Controllers and Memory Cards" documentation and was cross-checked against
// the approach of PCSX-Redux (GPL-2+) and Beetle PSX / Mednafen (GPL-2+); no code was copied.
//
// Model: no per-cycle ticking. Writing JOY_DATA (with TXEN set) schedules the end of the byte exchange at
// 8 * reload * factor cycles; at that point the addressed device computes its reply, the RX FIFO is pushed
// and, if the device acknowledges, /ACK goes low kAckDelay cycles later for kAckWidth cycles. The ACK IRQ
// (IRQ7, if CTRL bit 12 is set) fires on the falling edge.
//
// Extension point: DualShock / config-mode commands plug into Sio::pad_byte() (the pad step machine).

#include <array>
#include <cstdint>
#include <functional>
#include <span>

#include "dw3/bus.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"

namespace dw3 {

// Button bits of the 16-bit pad word. A set bit means "pressed" (Sio inverts it for the wire format).
enum PadButton : uint16_t {
    kPadSelect = 1u << 0, kPadL3 = 1u << 1, kPadR3 = 1u << 2, kPadStart = 1u << 3,
    kPadUp = 1u << 4, kPadRight = 1u << 5, kPadDown = 1u << 6, kPadLeft = 1u << 7,
    kPadL2 = 1u << 8, kPadR2 = 1u << 9, kPadL1 = 1u << 10, kPadR1 = 1u << 11,
    kPadTriangle = 1u << 12, kPadCircle = 1u << 13, kPadCross = 1u << 14, kPadSquare = 1u << 15,
};

// Supplies controller state; polled when a pad byte exchange completes. port is 0 or 1.
struct PadSource {
    virtual ~PadSource() = default;
    virtual uint16_t buttons(int port) = 0;  // bitmask of PadButton, 1 = pressed
    virtual bool connected(int port) = 0;
};

// Trivial settable implementation (both ports, connected flags default false).
struct SimplePads : PadSource {
    std::array<uint16_t, 2> pressed{};
    std::array<bool, 2> plugged{};
    uint16_t buttons(int port) override { return pressed[port & 1]; }
    bool connected(int port) override { return plugged[port & 1]; }
};

class Sio {
public:
    static constexpr uint32_t kBase = 0x1F801040u;
    static constexpr uint32_t kCardSize = 128 * 1024;
    static constexpr uint32_t kSectorSize = 128;
    static constexpr uint32_t kSectorCount = 1024;

    // Timing (CPU cycles).
    static constexpr int64_t kAckDelay = 100;  // end of byte -> /ACK low (approximation of ~3 us)
    static constexpr int64_t kAckWidth = 100;  // /ACK low duration

    // JOY_STAT bits.
    enum : uint32_t {
        kStatTxReady1 = 1u << 0, kStatRxNotEmpty = 1u << 1, kStatTxReady2 = 1u << 2, kStatParity = 1u << 3,
        kStatAckLow = 1u << 7, kStatIrq = 1u << 9,
    };
    // JOY_CTRL bits.
    enum : uint32_t {
        kCtrlTxEn = 1u << 0, kCtrlSelect = 1u << 1, kCtrlRxEn = 1u << 2, kCtrlAck = 1u << 4,
        kCtrlReset = 1u << 6, kCtrlTxIrq = 1u << 10, kCtrlRxIrq = 1u << 11, kCtrlAckIrq = 1u << 12,
        kCtrlPort = 1u << 13,
    };

    Sio(Bus& bus, Scheduler& sched, IrqController& irq);

    // Pad source per port (null = nothing plugged). Not owned.
    void set_pad_source(int port, PadSource* src) { pads_[port & 1] = src; }

    // Memory card storage per slot, exactly kCardSize bytes (not owned; the caller may back it with a file).
    // Returns false (and leaves the slot untouched) on a wrong size. A fresh card reports FLAG 0x08 until the
    // first successful write. detach_card() = no card inserted (no ACK, RX 0xFF).
    bool attach_card(int slot, std::span<uint8_t> storage);
    void detach_card(int slot);
    bool has_card(int slot) const { return !cards_[slot & 1].data.empty(); }
    uint8_t card_flag(int slot) const { return cards_[slot & 1].flag; }
    // The attached storage (empty span = no card); lets the native BIOS operate on the same image.
    std::span<uint8_t> card_data(int slot) const { return cards_[slot & 1].data; }
    // Clears the "new card" FLAG bit, as any successful write does.
    void clear_card_new(int slot) { cards_[slot & 1].flag &= static_cast<uint8_t>(~0x08); }
    // Called after each committed sector write (e.g. to mark the .mcd dirty).
    std::function<void(int slot, uint32_t sector)> on_card_write;

    void reset();  // power-on state (keeps attached devices)

    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t value, int size);

    // Introspection.
    uint32_t stat() const;
    uint16_t ctrl() const { return ctrl_; }
    uint16_t mode() const { return mode_; }
    uint16_t baud() const { return baud_; }
    int selected_port() const { return (ctrl_ & kCtrlPort) ? 1 : 0; }
    bool selected() const { return (ctrl_ & kCtrlSelect) != 0; }
    bool busy() const { return busy_; }
    std::size_t rx_count() const { return rx_count_; }
    int64_t byte_cycles() const;  // current byte transfer time

private:
    struct Card {
        std::span<uint8_t> data;
        uint8_t flag = 0x08;
    };
    enum class Dev : uint8_t { Idle, Pad, Card, Dead };

    uint32_t read_word(uint32_t off, int size);
    uint8_t pop_rx();
    void write_data(uint8_t v);
    void write_ctrl(uint16_t v);
    void start_tx(uint8_t v);
    void finish_tx(uint8_t tx);
    uint8_t device_byte(uint8_t tx, bool& ack);
    uint8_t pad_byte(uint8_t tx, bool& ack);
    uint8_t card_byte(uint8_t tx, bool& ack);
    void reset_device();
    void ack_begin();
    void set_irq();
    void push_rx(uint8_t b);

    Scheduler& sched_;
    IrqController& irq_;
    std::array<PadSource*, 2> pads_{};
    std::array<Card, 2> cards_{};

    // DualShock (SCPH-1200) per-port state. Starts in digital mode (ID 0x41), like a pad with the
    // analog LED off; libpad probes it with config-mode commands.
    struct DualShock {
        bool analog = false, config = false, locked = false;
        std::array<uint8_t, 6> rumble_map{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::array<uint8_t, 6> reply{};  // payload reply bytes for the current command
        int payload = 2;                 // payload length of the current command
        uint8_t config_pending = 0xFF;   // 0x43 parameter, applied when the transfer ends
        bool polling = false;            // current command returns button/stick data
    };
    std::array<DualShock, 2> ds_{};

public:
    bool pad_analog(int port) const { return ds_[port & 1].analog; }
    bool pad_config_mode(int port) const { return ds_[port & 1].config; }

private:

    uint16_t mode_ = 0, ctrl_ = 0, baud_ = 0;
    bool irq_flag_ = false, parity_ = false, ack_low_ = false, busy_ = false;
    std::array<uint8_t, 8> rx_{};
    std::size_t rx_head_ = 0, rx_count_ = 0;
    bool has_next_ = false;  // a byte waiting behind the one being shifted (or held while TXEN is off)
    uint8_t next_tx_ = 0;
    EventId xfer_ev_ = kNoEvent, ack_ev_ = kNoEvent, ack_end_ev_ = kNoEvent;

    // Device state machine (reset on deselect / port change).
    Dev dev_ = Dev::Idle;
    int step_ = 0;  // bytes handled so far, including the address byte
    uint8_t cmd_ = 0, msb_ = 0, lsb_ = 0, chk_ = 0;
    bool bad_checksum_ = false;
    uint32_t sector_ = 0;
    std::array<uint8_t, kSectorSize> buf_{};
};

}  // namespace dw3
