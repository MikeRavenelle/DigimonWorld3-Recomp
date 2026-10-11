#include "dw3/cdrom.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dw3 {

namespace {
bool bcd_ok(uint8_t b) { return (b & 0x0F) <= 9 && (b >> 4) <= 9; }
uint8_t to_bcd(unsigned v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
uint8_t from_bcd(uint8_t b) { return static_cast<uint8_t>((b >> 4) * 10 + (b & 0x0F)); }

// Accepted parameter count range per command; false for unknown commands.
bool param_range(uint8_t cmd, int& lo, int& hi) {
    lo = hi = 0;
    switch (cmd) {
        case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x0C: case 0x0F: case 0x10: case 0x11: case 0x13: case 0x15: case 0x16: case 0x1A: case 0x1B:
        case 0x1E:
            return true;
        case 0x02: lo = hi = 3; return true;
        case 0x03: hi = 1; return true;
        case 0x0D: lo = hi = 2; return true;
        case 0x0E: case 0x12: case 0x14: case 0x19: lo = hi = 1; return true;
        default: return false;
    }
}
}  // namespace

Cdrom::Cdrom(Bus& bus, Scheduler& sched, IrqController& irq, Dma& dma) : sched_(sched), irq_(irq), dma_(dma) {
    bus.map_io({kBase, 4, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
    dma_.attach(DmaChannel::Cdrom, this);
    reset();
}

void Cdrom::set_disc(SectorSource* disc) {
    disc_ = disc;
    stop_stream();
    motor_on_ = disc != nullptr;
    have_last_ = false;
}

void Cdrom::reset() {
    ++epoch_;
    sched_.cancel(sector_ev_);
    sched_.cancel(flush_ev_);
    sector_ev_ = flush_ev_ = kNoEvent;
    index_ = enable_ = flag_ = 0;
    line_ = busy_ = false;
    request_ = 0;
    params_.clear();
    resp_len_ = resp_pos_ = 0;
    queue_.clear();
    data_len_ = data_pos_ = 0;
    have_ready_ = false;
    volume_ = pending_volume_ = Volume{};
    adpcm_mute_ = muted_ = false;
    sound_map_ = sound_coding_ = 0;
    state_ = State::Idle;
    stream_play_ = false;
    motor_on_ = disc_ != nullptr;
    mode_ = filter_file_ = filter_chan_ = 0;
    lba_ = last_lba_ = 0;
    setloc_pending_ = false;
    have_last_ = false;
    overruns_ = 0;
}

uint8_t Cdrom::stat() const {
    uint8_t s = 0;
    if (motor_on_) s |= kStatMotor;
    if (shell_open_) s |= kStatShellOpen;
    switch (state_) {
        case State::Seeking: s |= kStatSeek; break;
        case State::Reading: s |= kStatRead; break;
        case State::Playing: s |= kStatPlay; break;
        default: break;
    }
    return s;
}

int64_t Cdrom::sector_total() const { return disc_ ? static_cast<int64_t>(disc_->sector_count()) : 0; }

int64_t Cdrom::seek_cycles(int64_t from, int64_t to) {
    int64_t d = std::min<int64_t>(std::llabs(to - from), kSeekMaxSectors);
    return kSeekBase + d * kSeekPerSector;
}

// ---- register access ----

uint8_t Cdrom::status_reg() const {
    uint8_t s = index_ & 3;
    if (params_.empty()) s |= kStsParamEmpty;
    if (params_.size() < 16) s |= kStsParamReady;
    if (resp_pos_ < resp_len_) s |= kStsRespReady;
    if (data_pos_ < data_len_) s |= kStsDataReq;
    if (busy_) s |= kStsBusy;
    return s;
}

uint8_t Cdrom::data_byte() {
    // Reading past the end yields zeros (hardware repeats stale FIFO contents; games never rely on it).
    if (data_pos_ >= data_len_) return 0;
    return data_[data_pos_++];
}

uint8_t Cdrom::read8(uint32_t off) {
    switch (off & 3) {
        case 0: return status_reg();
        case 1: return resp_pos_ < resp_len_ ? resp_[resp_pos_++] : uint8_t{0};
        case 2: return data_byte();
        default: return (index_ & 1) ? static_cast<uint8_t>(0xE0 | (flag_ & 0x1F)) : static_cast<uint8_t>(0xE0 | enable_);
    }
}

uint32_t Cdrom::read(uint32_t off, int size) {
    off &= 3;
    if (off == 2 && size > 1) {  // data FIFO accessed as halfword / word
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v |= static_cast<uint32_t>(data_byte()) << (8 * i);
        return v;
    }
    uint32_t v = 0;
    for (int i = 0; i < size && off + i < 4; ++i) v |= static_cast<uint32_t>(read8(off + i)) << (8 * i);
    return v;
}

void Cdrom::write(uint32_t off, uint32_t value, int size) {
    off &= 3;
    for (int i = 0; i < size && off + i < 4; ++i) write8(off + i, static_cast<uint8_t>(value >> (8 * i)));
}

void Cdrom::load_data_fifo() {
    data_pos_ = 0;
    data_len_ = 0;
    if (!have_ready_) return;
    if (mode_ & kModeWholeSector) {
        std::memcpy(data_.data(), ready_.data() + 12, 0x924);
        data_len_ = 0x924;
    } else {
        std::memcpy(data_.data(), ready_.data() + 24, 0x800);
        data_len_ = 0x800;
    }
}

void Cdrom::write_flag(uint8_t v) {
    flag_ &= static_cast<uint8_t>(~(v & 0x1F));
    if (v & 0x40) params_.clear();
    update_line();
    if (flag_ == 0 && !queue_.empty()) schedule_flush();
}

void Cdrom::write8(uint32_t off, uint8_t v) {
    switch (off & 3) {
        case 0: index_ = v & 3; break;
        case 1:
            switch (index_) {
                case 0: start_command(v); break;
                case 1: sound_map_ = v; break;
                case 2: sound_coding_ = v; break;
                default: pending_volume_.rr = v; break;
            }
            break;
        case 2:
            switch (index_) {
                case 0:
                    if (params_.size() < 16) params_.push_back(v);
                    break;
                case 1:
                    enable_ = v & 0x1F;
                    update_line();
                    break;
                case 2: pending_volume_.ll = v; break;
                default: pending_volume_.rl = v; break;
            }
            break;
        default:
            switch (index_) {
                case 0:
                    request_ = v;
                    // BFRD loads the next sector only into an empty FIFO; re-asserting it mid-sector (libcd
                    // does this between its header and data DMAs) must not rewind the read position.
                    if (v & 0x80) {
                        if (data_pos_ >= data_len_) load_data_fifo();
                    } else {
                        data_len_ = data_pos_ = 0;
                    }
                    break;
                case 1: write_flag(v); break;
                case 2: pending_volume_.lr = v; break;
                default:
                    adpcm_mute_ = (v & 1) != 0;
                    if (v & 0x20) volume_ = pending_volume_;
                    break;
            }
            break;
    }
}

void Cdrom::dma_read(std::span<uint32_t> out) {
    for (auto& w : out) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_byte()) << (8 * i);
        w = v;
    }
}

// ---- interrupt / response plumbing ----

void Cdrom::update_line() {
    bool now = (flag_ & enable_ & 0x1F) != 0;
    if (now && !line_) irq_.raise(IrqSource::Cdrom);
    line_ = now;
}

void Cdrom::later(int64_t delay, std::function<void()> fn) {
    uint64_t ep = epoch_;
    sched_.schedule_in(delay, [this, ep, fn = std::move(fn)](int64_t) {
        if (ep == epoch_) fn();
    });
}

void Cdrom::deliver(Resp r) {
    resp_len_ = static_cast<uint8_t>(std::min<size_t>(r.data.size(), resp_.size()));
    std::copy_n(r.data.begin(), resp_len_, resp_.begin());
    resp_pos_ = 0;
    flag_ = r.int_no & 7;
    if (r.first) busy_ = false;
    update_line();
}

// Responses are delivered only while no interrupt is unacknowledged; otherwise they wait (in order).
void Cdrom::enqueue(uint8_t int_no, std::vector<uint8_t> data, bool first) {
    Resp r{int_no, std::move(data), first};
    if (flag_ == 0 && queue_.empty()) deliver(std::move(r));
    else queue_.push_back(std::move(r));
}

void Cdrom::schedule_flush() {
    if (sched_.is_pending(flush_ev_)) return;
    uint64_t ep = epoch_;
    flush_ev_ = sched_.schedule_in(kAckGap, [this, ep](int64_t) {
        flush_ev_ = kNoEvent;
        if (ep != epoch_ || flag_ != 0 || queue_.empty()) return;
        Resp r = std::move(queue_.front());
        queue_.pop_front();
        deliver(std::move(r));
    });
}

// ---- commands ----

void Cdrom::start_command(uint8_t cmd) {
    if (busy_) return;  // the controller ignores commands while it is still processing one
    busy_ = true;
    std::vector<uint8_t> p = std::move(params_);
    params_.clear();
    later(kFirstResponse, [this, cmd, p = std::move(p)] { execute(cmd, p); });
}

void Cdrom::execute(uint8_t cmd, const std::vector<uint8_t>& p) {
    const uint8_t st = stat();
    auto first = [&](std::vector<uint8_t> d) { enqueue(3, std::move(d), true); };
    auto fail = [&](uint8_t code) { enqueue(5, {static_cast<uint8_t>(st | kStatError), code}, true); };
    auto second_ok = [&](int64_t delay) { later(delay, [this] { enqueue(2, {stat()}); }); };
    auto second_err = [&](int64_t delay, uint8_t code) {
        later(delay, [this, code] { enqueue(5, {static_cast<uint8_t>(stat() | kStatError), code}); });
    };

    int lo, hi;
    if (!param_range(cmd, lo, hi)) return fail(kErrBadCommand);
    if (static_cast<int>(p.size()) < lo || static_cast<int>(p.size()) > hi) return fail(kErrBadParamCount);
    const bool ready = disc_ != nullptr && !shell_open_;

    switch (cmd) {
        case 0x01: first({st}); break;  // Getstat
        case 0x02: {                    // Setloc
            if (!bcd_ok(p[0]) || !bcd_ok(p[1]) || !bcd_ok(p[2]) || from_bcd(p[1]) >= 60 || from_bcd(p[2]) >= 75)
                return fail(kErrBadParam);
            setloc_lba_ = (from_bcd(p[0]) * 60 + from_bcd(p[1])) * 75 + from_bcd(p[2]) - 150;
            setloc_pending_ = true;
            first({st});
            break;
        }
        case 0x03:  // Play (stub: no audio tracks; spins and advances the position silently)
            first({st});
            if (!ready) { second_err(1000, kErrNotReady); break; }
            start_stream(true);
            break;
        case 0x04: case 0x05: first({st}); break;  // Forward / Backward (stub)
        case 0x06: case 0x1B:                      // ReadN / ReadS
            first({st});
            if (!ready) { second_err(1000, kErrNotReady); break; }
            start_stream(false);
            break;
        case 0x07:  // MotorOn
            if (motor_on_) return fail(kErrBadParamCount);
            first({st});
            later(kMotorOnDelay, [this] {
                motor_on_ = true;
                enqueue(2, {stat()});
            });
            break;
        case 0x08:  // Stop
            first({st});
            stop_stream();
            later(motor_on_ ? kStopDelay : kPauseIdle, [this] {
                motor_on_ = false;
                enqueue(2, {stat()});
            });
            break;
        case 0x09: {  // Pause
            first({st});
            bool active = state_ == State::Reading || state_ == State::Playing || state_ == State::Seeking;
            stop_stream();
            second_ok(active ? ((mode_ & kModeDouble) ? kPause2x : kPause1x) : kPauseIdle);
            break;
        }
        case 0x0A:  // Init
            first({st});
            stop_stream();
            mode_ = 0;
            muted_ = false;
            setloc_pending_ = false;
            later(kInitDelay, [this] {
                motor_on_ = disc_ != nullptr;
                enqueue(2, {stat()});
            });
            break;
        case 0x0B: muted_ = true; first({st}); break;
        case 0x0C: muted_ = false; first({st}); break;
        case 0x0D:
            filter_file_ = p[0];
            filter_chan_ = p[1];
            first({st});
            break;
        case 0x0E: mode_ = p[0]; first({st}); break;
        case 0x0F: first({st, mode_, 0, filter_file_, filter_chan_}); break;
        case 0x10: {  // GetlocL
            if (!have_last_ || state_ != State::Reading) return fail(kErrNotReady);
            std::vector<uint8_t> d(last_hdr_.begin(), last_hdr_.end());
            first(std::move(d));
            break;
        }
        case 0x11: {  // GetlocP
            int64_t pos = std::max<int64_t>(last_lba_, 0);
            unsigned rel = static_cast<unsigned>(pos);
            unsigned abs = static_cast<unsigned>(pos + 150);
            first({0x01, 0x01, to_bcd(rel / 75 / 60), to_bcd(rel / 75 % 60), to_bcd(rel % 75),
                   to_bcd(abs / 75 / 60), to_bcd(abs / 75 % 60), to_bcd(abs % 75)});
            break;
        }
        case 0x12:  // SetSession
            if (p[0] == 0) return fail(kErrBadParam);
            first({st});
            if (p[0] == 1) second_ok(100000);
            else second_err(100000, kErrBadCommand);
            break;
        case 0x13: first({st, 0x01, 0x01}); break;  // GetTN
        case 0x14: {                                // GetTD
            if (!ready) return fail(kErrNotReady);
            if (!bcd_ok(p[0])) return fail(kErrBadParam);
            uint8_t track = from_bcd(p[0]);
            if (track == 0) {
                unsigned total = static_cast<unsigned>(sector_total()) + 150;
                first({st, to_bcd(total / 75 / 60), to_bcd(total / 75 % 60)});
            } else if (track == 1) {
                first({st, 0x00, 0x02});
            } else {
                return fail(kErrBadParam);
            }
            break;
        }
        case 0x15: case 0x16: {  // SeekL / SeekP
            first({st});
            if (!ready) { second_err(1000, kErrNotReady); break; }
            stop_stream();
            int64_t target = setloc_pending_ ? setloc_lba_ : lba_;
            int64_t delay = seek_cycles(last_lba_, target);
            setloc_pending_ = false;
            if (target < 0 || target >= sector_total()) {
                second_err(delay, kErrSeek);
                break;
            }
            state_ = State::Seeking;
            later(delay, [this, target] {
                state_ = State::Idle;
                lba_ = last_lba_ = target;
                enqueue(2, {stat()});
            });
            break;
        }
        case 0x19:  // Test
            switch (p[0]) {
                case 0x20: first({0x94, 0x09, 0x19, 0xC0}); break;
                case 0x04: first({st}); break;
                case 0x05: first({0x00, 0x00}); break;
                default: return fail(kErrBadParam);
            }
            break;
        case 0x1A:  // GetID
            first({st});
            if (!ready) {
                later(kGetIdDelay, [this] { enqueue(5, {0x08, 0x40, 0, 0, 0, 0, 0, 0}); });
            } else {
                later(kGetIdDelay, [this] {
                    enqueue(2, {stat(), 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'});
                });
            }
            break;
        case 0x1E:  // ReadTOC
            first({st});
            if (!ready) second_err(1000, kErrNotReady);
            else second_ok(kReadTocDelay);
            break;
        default: return fail(kErrBadCommand);
    }
}

// ---- streaming ----

void Cdrom::stop_stream() {
    sched_.cancel(sector_ev_);
    sector_ev_ = kNoEvent;
    state_ = State::Idle;
}

void Cdrom::start_stream(bool play) {
    bool continuing = !setloc_pending_ && sched_.is_pending(sector_ev_) && stream_play_ == play;
    if (continuing) return;  // already streaming from the right place
    sched_.cancel(sector_ev_);
    int64_t delay = 0;
    if (setloc_pending_) {
        delay = seek_cycles(last_lba_, setloc_lba_);
        lba_ = setloc_lba_;
        setloc_pending_ = false;
    }
    if (!motor_on_) {
        delay += kSpinUp;
        motor_on_ = true;
    }
    stream_play_ = play;
    state_ = State::Seeking;
    sector_ev_ = sched_.schedule_in(delay + period(), [this](int64_t due) { sector_tick(due); });
}

// Applies the XA-ADPCM rules; true if the sector was consumed (not delivered as data).
bool Cdrom::route_xa(const uint8_t* raw) {
    if (!(mode_ & kModeXa) || raw[15] != 2) return false;
    uint8_t sm = raw[18];
    if ((sm & 0x44) != 0x44) return false;  // needs audio (bit 2) + real-time (bit 6)
    if ((mode_ & kModeFilter) && (raw[16] != filter_file_ || raw[17] != filter_chan_)) return true;  // dropped
    if (xa_sink_) xa_sink_->xa_sector(raw);
    return true;
}

void Cdrom::sector_tick(int64_t due) {
    sector_ev_ = kNoEvent;
    state_ = stream_play_ ? State::Playing : State::Reading;
    auto halt = [&](uint8_t int_no, uint8_t code) {
        std::vector<uint8_t> d{static_cast<uint8_t>(stat() | (int_no == 5 ? kStatError : 0))};
        if (int_no == 5) d.push_back(code);
        state_ = State::Idle;
        enqueue(int_no, std::move(d));
    };
    if (lba_ >= sector_total()) return halt(4, 0);  // INT4: end of data
    if (lba_ < 0 || !disc_ || !disc_->read_raw(static_cast<uint32_t>(lba_), raw_.data())) return halt(5, kErrSeek);

    last_lba_ = lba_++;
    have_last_ = true;
    std::copy_n(raw_.begin() + 12, 8, last_hdr_.begin());
    sector_ev_ = sched_.schedule(due + period(), [this](int64_t d) { sector_tick(d); });
    if (stream_play_ || route_xa(raw_.data())) return;

    // A new data sector can only be announced when the previous interrupt was acknowledged and nothing is
    // waiting; otherwise the drive's small buffer overruns and the sector is dropped (counted).
    if (flag_ != 0 || !queue_.empty()) {
        ++overruns_;
        return;
    }
    ready_ = raw_;
    have_ready_ = true;
    enqueue(1, {stat()});
}

}  // namespace dw3
