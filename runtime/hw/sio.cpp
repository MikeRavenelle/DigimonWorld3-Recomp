#include "dw3/sio.h"

#include <algorithm>
#include <cstring>

namespace dw3 {

Sio::Sio(Bus& bus, Scheduler& sched, IrqController& irq) : sched_(sched), irq_(irq) {
    bus.map_io({kBase, 0x10, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
    reset();
}

bool Sio::attach_card(int slot, std::span<uint8_t> storage) {
    if (storage.size() != kCardSize) return false;
    cards_[slot & 1] = Card{storage, 0x08};
    return true;
}

void Sio::detach_card(int slot) { cards_[slot & 1] = Card{}; }

void Sio::reset() {
    sched_.cancel(xfer_ev_);
    sched_.cancel(ack_ev_);
    sched_.cancel(ack_end_ev_);
    xfer_ev_ = ack_ev_ = ack_end_ev_ = kNoEvent;
    ctrl_ = 0;
    irq_flag_ = parity_ = ack_low_ = busy_ = has_next_ = false;
    rx_head_ = rx_count_ = 0;
    reset_device();
}

void Sio::reset_device() {
    dev_ = Dev::Idle;
    step_ = 0;
}

int64_t Sio::byte_cycles() const {
    static constexpr int kFactor[4] = {1, 1, 16, 64};
    return 8 * static_cast<int64_t>(std::max<uint16_t>(baud_, 1)) * kFactor[mode_ & 3];
}

uint32_t Sio::stat() const {
    uint32_t s = 0;
    if (!has_next_) s |= kStatTxReady1;
    if (rx_count_) s |= kStatRxNotEmpty;
    if (!busy_) s |= kStatTxReady2;
    if (parity_) s |= kStatParity;
    if (ack_low_) s |= kStatAckLow;
    if (irq_flag_) s |= kStatIrq;
    return s;  // baud timer (bits 11-31) approximated as 0
}

uint8_t Sio::pop_rx() {
    if (!rx_count_) return 0xFF;
    uint8_t b = rx_[rx_head_];
    rx_head_ = (rx_head_ + 1) % rx_.size();
    --rx_count_;
    return b;
}

void Sio::push_rx(uint8_t b) {
    if (rx_count_ == rx_.size()) return;  // overflow: new byte dropped
    rx_[(rx_head_ + rx_count_) % rx_.size()] = b;
    ++rx_count_;
}

void Sio::set_irq() {
    if (irq_flag_) return;
    irq_flag_ = true;
    irq_.raise(IrqSource::PadMemcard);
}

// ---- register access ----

// Reads the aligned 32-bit window containing off. JOY_DATA pops the FIFO once per access.
uint32_t Sio::read_word(uint32_t off, int size) {
    switch (off & 0xC) {
        case 0x0: return pop_rx();  // wider reads: only the head byte is returned, upper bytes 0
        case 0x4: return stat();
        case 0x8: return mode_ | (static_cast<uint32_t>(ctrl_) << 16);
        default: return static_cast<uint32_t>(baud_) << 16;
    }
    (void)size;
}

uint32_t Sio::read(uint32_t off, int size) {
    off &= 0xF;
    uint32_t w = read_word(off, size);
    if (size >= 4) return w;
    uint32_t sh = (off & 3) * 8;
    return (w >> sh) & (size == 2 ? 0xFFFFu : 0xFFu);
}

void Sio::write(uint32_t off, uint32_t value, int size) {
    off &= 0xF;
    if (off < 4) {  // JOY_DATA: hardware pushes the low byte only
        write_data(static_cast<uint8_t>(value));
        return;
    }
    if (off < 8) return;  // JOY_STAT is read-only
    // Halfword registers: MODE 0x8, CTRL 0xA, (0xC unused), BAUD 0xE. Merge narrow writes into the old value.
    for (int i = 0; i < size;) {
        uint32_t o = off + i;
        uint32_t reg = o & ~1u;
        uint16_t cur = 0;
        switch (reg) {
            case 0x8: cur = mode_; break;
            case 0xA: cur = ctrl_; break;
            case 0xE: cur = baud_; break;
            default: break;
        }
        uint32_t sh = 8 * i;
        uint16_t nv;
        int used;
        if ((o & 1) == 0 && i + 2 <= size) {
            nv = static_cast<uint16_t>(value >> sh);
            used = 2;
        } else if (o & 1) {
            nv = static_cast<uint16_t>((cur & 0x00FF) | ((value >> sh) & 0xFF) << 8);
            used = 1;
        } else {
            nv = static_cast<uint16_t>((cur & 0xFF00) | ((value >> sh) & 0xFF));
            used = 1;
        }
        if (reg == 0x8) mode_ = nv;
        else if (reg == 0xA) write_ctrl(nv);
        else if (reg == 0xE) baud_ = nv;
        i += used;
    }
}

void Sio::write_ctrl(uint16_t v) {
    if (v & kCtrlReset) {
        reset();
        return;
    }
    if (v & kCtrlAck) {
        irq_flag_ = false;
        parity_ = false;
    }
    uint16_t old = ctrl_;
    ctrl_ = v & ~(kCtrlAck | kCtrlReset);
    bool port_changed = ((old ^ ctrl_) & kCtrlPort) != 0;
    bool sel_changed = ((old ^ ctrl_) & kCtrlSelect) != 0;
    if (port_changed || sel_changed) {
        // /JOYn level or port changed: devices restart their command state and any pending ACK is dropped.
        sched_.cancel(ack_ev_);
        sched_.cancel(ack_end_ev_);
        ack_ev_ = ack_end_ev_ = kNoEvent;
        ack_low_ = false;
        reset_device();
    }
    if ((ctrl_ & kCtrlTxEn) && has_next_ && !busy_) {
        has_next_ = false;
        start_tx(next_tx_);
    }
}

void Sio::write_data(uint8_t v) {
    if (busy_ || !(ctrl_ & kCtrlTxEn)) {
        next_tx_ = v;  // one-deep holding register
        has_next_ = true;
        return;
    }
    start_tx(v);
}

// ---- transfers ----

void Sio::start_tx(uint8_t v) {
    busy_ = true;
    xfer_ev_ = sched_.schedule_in(byte_cycles(), [this, v](int64_t) { finish_tx(v); });
}

void Sio::finish_tx(uint8_t tx) {
    xfer_ev_ = kNoEvent;
    busy_ = false;
    bool ack = false;
    uint8_t rx = 0xFF;
    if (selected()) rx = device_byte(tx, ack);
    push_rx(rx);
    static constexpr std::size_t kThreshold[4] = {1, 2, 4, 8};
    if ((ctrl_ & kCtrlRxIrq) && rx_count_ >= kThreshold[(ctrl_ >> 8) & 3]) set_irq();
    if (ctrl_ & kCtrlTxIrq) set_irq();
    if (ack) ack_ev_ = sched_.schedule_in(kAckDelay, [this](int64_t) { ack_begin(); });
    if (has_next_ && (ctrl_ & kCtrlTxEn)) {
        has_next_ = false;
        start_tx(next_tx_);
    }
}

void Sio::ack_begin() {
    ack_ev_ = kNoEvent;
    ack_low_ = true;
    if (ctrl_ & kCtrlAckIrq) set_irq();
    ack_end_ev_ = sched_.schedule_in(kAckWidth, [this](int64_t) {
        ack_end_ev_ = kNoEvent;
        ack_low_ = false;
    });
}

uint8_t Sio::device_byte(uint8_t tx, bool& ack) {
    ack = false;
    if (dev_ == Dev::Idle) {
        int port = selected_port();
        if (tx == 0x01 && pads_[port] && pads_[port]->connected(port)) dev_ = Dev::Pad;
        else if (tx == 0x81 && has_card(port)) dev_ = Dev::Card;
        else dev_ = Dev::Dead;
        step_ = 1;
        ack = dev_ != Dev::Dead;
        return 0xFF;
    }
    if (dev_ == Dev::Pad) return pad_byte(tx, ack);
    if (dev_ == Dev::Card) return card_byte(tx, ack);
    return 0xFF;
}

// DualShock pad (psx-spx "Controllers and Memory Cards - Configuration Commands"). Byte s=1 is the
// command (reply: ID low byte), s=2 the 0x5A marker, s>=3 the payload. ACK after every byte but the last.
uint8_t Sio::pad_byte(uint8_t tx, bool& ack) {
    int port = selected_port();
    DualShock& d = ds_[static_cast<std::size_t>(port)];
    int s = step_++;
    ack = true;
    auto last = [&](uint8_t v) {
        ack = false;
        dev_ = Dev::Dead;
        return v;
    };
    if (s == 1) {
        cmd_ = tx;
        uint16_t wire = static_cast<uint16_t>(~pads_[port]->buttons(port));
        std::array<uint8_t, 6> poll{static_cast<uint8_t>(wire), static_cast<uint8_t>(wire >> 8), 0x80, 0x80, 0x80,
                                    0x80};  // buttons, then right/left sticks centred
        uint8_t id = d.config ? 0xF3 : d.analog ? 0x73 : 0x41;
        d.payload = d.config ? 6 : d.analog ? 6 : 2;
        d.reply = {};
        d.polling = false;
        bool config_cmd = d.config && tx >= 0x40 && tx <= 0x4F;
        if (tx == 0x42 || (tx == 0x43 && !d.config) || (!d.config && tx != 0x42 && tx != 0x43)) {
            d.reply = poll;  // poll (outside config mode every command behaves like 0x42)
            d.polling = true;
        } else if (config_cmd) {
            switch (tx) {
                case 0x45: d.reply = {0x01, 0x02, static_cast<uint8_t>(d.analog ? 1 : 0), 0x02, 0x01, 0x00}; break;
                case 0x47: d.reply = {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}; break;
                case 0x4D: d.reply = d.rumble_map; break;
                default: break;  // 0x43 (in config), 0x44, 0x46/0x4C (filled from the parameter byte), others: zeros
            }
        }
        return id;
    }
    if (s == 2) return 0x5A;
    int k = s - 3;  // payload index
    if (d.polling && k < 2) {  // buttons are sampled as their byte is shifted out
        uint16_t wire = static_cast<uint16_t>(~pads_[port]->buttons(port));
        d.reply[static_cast<std::size_t>(k)] = static_cast<uint8_t>(k == 0 ? wire : wire >> 8);
    }
    // Parameter-dependent replies and state changes.
    if (cmd_ == 0x43 && k == 0) d.config_pending = tx;
    if (d.config && cmd_ == 0x44) {
        if (k == 0 && !d.locked) d.analog = tx == 0x01;
        if (k == 1) d.locked = tx == 0x03;
    }
    if (d.config && cmd_ == 0x46 && k == 0) {
        d.reply = tx == 0 ? std::array<uint8_t, 6>{0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}
                          : std::array<uint8_t, 6>{0x00, 0x00, 0x01, 0x01, 0x01, 0x14};
    }
    if (d.config && cmd_ == 0x4C && k == 0) d.reply = {0x00, 0x00, 0x00, static_cast<uint8_t>(tx == 0 ? 4 : 7), 0x00, 0x00};
    uint8_t v = d.reply[static_cast<std::size_t>(std::min(k, 5))];
    if (d.config && cmd_ == 0x4D && k < 6) d.rumble_map[static_cast<std::size_t>(k)] = tx;
    if (k + 1 >= d.payload) {
        if (cmd_ == 0x43 && d.config_pending <= 1) d.config = d.config_pending == 1;
        d.config_pending = 0xFF;
        return last(v);
    }
    return v;
}

// Memory card commands: 0x52 read, 0x57 write, 0x53 get ID. s counts bytes handled including 0x81 (s == 0).
uint8_t Sio::card_byte(uint8_t tx, bool& ack) {
    Card& card = cards_[selected_port()];
    int s = step_++;
    ack = true;
    auto dead = [&](uint8_t v) {
        ack = false;
        dev_ = Dev::Dead;
        return v;
    };
    if (s == 1) {
        cmd_ = tx;
        if (tx != 0x52 && tx != 0x57 && tx != 0x53) return dead(0xFF);  // unknown command: card goes silent
        return card.flag;
    }
    if (s == 2) return 0x5A;
    if (s == 3) return 0x5D;

    if (cmd_ == 0x53) {  // get ID: 5C 5D 04 00 00 80
        static constexpr uint8_t kId[6] = {0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
        uint8_t v = kId[s - 4];
        return s == 9 ? dead(v) : v;
    }

    auto sector_data = [&]() { return card.data.data() + static_cast<std::size_t>(sector_) * kSectorSize; };
    bool valid = sector_ < kSectorCount;

    if (cmd_ == 0x52) {  // read
        switch (s) {
            case 4: msb_ = tx; return 0x00;
            case 5:
                lsb_ = tx;
                sector_ = (static_cast<uint32_t>(msb_) << 8) | lsb_;
                chk_ = msb_ ^ lsb_;
                return msb_;
            case 6: return 0x5C;
            case 7: return 0x5D;
            case 8: return sector_ < kSectorCount ? msb_ : 0xFF;
            case 9:
                if (!valid) return dead(0xFF);  // bad sector: confirmed as FFFF, transfer ends
                return lsb_;
            default: break;
        }
        if (s < 10 + static_cast<int>(kSectorSize)) {
            uint8_t b = sector_data()[s - 10];
            chk_ ^= b;
            return b;
        }
        if (s == 10 + static_cast<int>(kSectorSize)) return chk_;
        return dead(0x47);  // 'G'
    }

    // write
    switch (s) {
        case 4: msb_ = tx; return 0x00;
        case 5:
            lsb_ = tx;
            sector_ = (static_cast<uint32_t>(msb_) << 8) | lsb_;
            chk_ = msb_ ^ lsb_;
            return msb_;
        default: break;
    }
    if (s < 6 + static_cast<int>(kSectorSize)) {
        int i = s - 6;
        uint8_t prev = i == 0 ? lsb_ : buf_[i - 1];
        buf_[i] = tx;
        chk_ ^= tx;
        return prev;
    }
    int e = s - (6 + static_cast<int>(kSectorSize));
    if (e == 0) {
        bad_checksum_ = tx != chk_;
        return buf_[kSectorSize - 1];
    }
    if (e == 1) return 0x5C;
    if (e == 2) return 0x5D;
    if (!valid) return dead(0xFF);
    if (bad_checksum_) return dead(0x4E);
    std::memcpy(sector_data(), buf_.data(), kSectorSize);
    card.flag &= static_cast<uint8_t>(~0x08);
    if (on_card_write) on_card_write(selected_port(), sector_);
    return dead(0x47);
}

}  // namespace dw3
