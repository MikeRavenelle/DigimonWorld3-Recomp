// SPU sample engine: voices, envelopes, mixer, capture buffers, reverb hookup and timed generation.
// Behaviour follows psx-spx "Sound Processing Unit (SPU)"; see spu.cpp for the other references.

#include <algorithm>

#include "dw3/spu.h"

namespace dw3 {

using namespace spu_dsp;

void Spu::check_irq_range(uint32_t addr, uint32_t len) {
    uint16_t cnt = regs_[kSpuCnt >> 1];
    if (!(cnt & (1u << 6)) || (regs_[kSpuStat >> 1] & (1u << 6))) return;
    uint32_t irq_addr = static_cast<uint32_t>(regs_[kIrqAddr >> 1]) * 8;
    if (irq_addr >= addr && irq_addr < addr + len) {
        regs_[kSpuStat >> 1] |= 1u << 6;
        irq_.raise(IrqSource::Spu);
    }
}

void Spu::Sweep::tick() {
    if (!(reg & 0x8000)) return;
    EnvRate r;
    r.exponential = reg & 0x4000;
    r.decreasing = reg & 0x2000;
    r.phase_negative = reg & 0x1000;
    r.shift = (reg >> 2) & 0x1F;
    r.step = reg & 3;
    r.never_steps = (reg & 0x7F) == 0x7F;
    envelope_tick(level, counter, r);
}

void Spu::key_on(uint32_t mask) {
    if (!mask) return;
    sync();
    for (int i = 0; i < kVoices; ++i) {
        if (!(mask >> i & 1)) continue;
        Voice& v = voices_[static_cast<std::size_t>(i)];
        v.addr = (static_cast<uint32_t>(vreg(i, 3)) * 8) & (kRamSize - 1);
        v.repeat = v.addr;
        v.counter = 0;
        v.buf.fill(0);
        v.s1 = v.s2 = 0;
        v.phase = Phase::Attack;
        v.level = 0;
        v.adsr_counter = 0;
        endx_ &= ~(1u << i);
        key_on_ |= 1u << i;
        groups_[static_cast<std::size_t>(i)] = classifier_ ? classifier_->classify(i) : VoiceGroup::Music;
        decode_block(v);
    }
}

void Spu::key_off(uint32_t mask) {
    if (!mask) return;
    sync();
    for (int i = 0; i < kVoices; ++i) {
        if (!(mask >> i & 1)) continue;
        Voice& v = voices_[static_cast<std::size_t>(i)];
        if (v.phase != Phase::Off) v.phase = Phase::Release;
        v.adsr_counter = 0;
    }
    key_on_ &= ~mask;
}

void Spu::decode_block(Voice& v) {
    uint8_t blk[16];
    for (uint32_t i = 0; i < 16; ++i) blk[i] = ram_[(v.addr + i) & (kRamSize - 1)];
    std::copy(v.buf.begin() + 28, v.buf.end(), v.buf.begin());
    decode_adpcm_block(blk, v.buf.data() + 3, v.s1, v.s2);
    v.flags = blk[1];
    if (v.flags & kFlagLoopStart) v.repeat = v.addr;
    check_irq_range(v.addr, 16);
}

void Spu::next_block(Voice& v, int index) {
    if (v.flags & kFlagLoopEnd) {
        endx_ |= 1u << index;
        v.addr = v.repeat;
        if (!(v.flags & kFlagLoopRepeat)) {  // end + mute
            v.phase = Phase::Release;
            v.level = 0;
            key_on_ &= ~(1u << index);
        }
    } else {
        v.addr = (v.addr + 16) & (kRamSize - 1);
    }
    decode_block(v);
}

void Spu::envelope(Voice& v, uint16_t adsr1, uint16_t adsr2) {
    EnvRate r;
    switch (v.phase) {
        case Phase::Off: return;
        case Phase::Attack:
            r.exponential = adsr1 & 0x8000;
            r.shift = (adsr1 >> 10) & 0x1F;
            r.step = (adsr1 >> 8) & 3;
            r.never_steps = ((r.step | (r.shift << 2)) == 0x7F);
            envelope_tick(v.level, v.adsr_counter, r);
            if (v.level >= 0x7FFF) v.phase = Phase::Decay;
            break;
        case Phase::Decay:
            r.exponential = true;
            r.decreasing = true;
            r.shift = (adsr1 >> 4) & 0xF;
            envelope_tick(v.level, v.adsr_counter, r);
            if (v.level <= static_cast<int32_t>((adsr1 & 0xF) + 1) * 0x800) v.phase = Phase::Sustain;
            break;
        case Phase::Sustain:
            r.exponential = adsr2 & 0x8000;
            r.decreasing = adsr2 & 0x4000;
            r.shift = (adsr2 >> 8) & 0x1F;
            r.step = (adsr2 >> 6) & 3;
            r.never_steps = ((r.step | (r.shift << 2)) == 0x7F);
            envelope_tick(v.level, v.adsr_counter, r);
            break;
        case Phase::Release:
            r.exponential = adsr2 & 0x20;
            r.decreasing = true;
            r.shift = adsr2 & 0x1F;
            r.never_steps = r.shift == 0x1F;
            envelope_tick(v.level, v.adsr_counter, r);
            if (v.level <= 0) v.phase = Phase::Off;
            break;
    }
}

void Spu::generate_sample() {
    const uint16_t cnt = regs_[kSpuCnt >> 1];
    noise_.tick((cnt >> 10) & 0xF, (cnt >> 8) & 3);
    auto mask32 = [&](uint32_t off) {
        return static_cast<uint32_t>(regs_[off >> 1]) | (static_cast<uint32_t>(regs_[(off >> 1) + 1] & 0xFF) << 16);
    };
    const uint32_t pmon = mask32(kPitchMod), non = mask32(kNoiseOn), eon = mask32(kReverbOn);

    if (groups_stale_) {
        groups_stale_ = false;
        for (int i = 0; i < kVoices; ++i)
            groups_[static_cast<std::size_t>(i)] = classifier_ ? classifier_->classify(i) : VoiceGroup::Music;
    }

    int32_t sum_l = 0, sum_r = 0, rev_l = 0, rev_r = 0;
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[static_cast<std::size_t>(i)];
        int32_t s = 0;
        if (v.level != 0) {
            int32_t raw = (non >> i & 1) ? static_cast<int16_t>(noise_.level)
                                         : gauss_interpolate(&v.buf[v.counter >> 12], v.counter >> 4);
            s = (raw * v.level) >> 15;
        }
        v.out = static_cast<int16_t>(s);
        if (s) {
            int32_t l = (s * v.vol[0].level) >> 15, r = (s * v.vol[1].level) >> 15;
            const int32_t gain = group_gain_[static_cast<std::size_t>(groups_[static_cast<std::size_t>(i)])];
            if (gain != 256) {
                l = (l * gain) >> 8;
                r = (r * gain) >> 8;
            }
            sum_l += l;
            sum_r += r;
            if (eon >> i & 1) {
                rev_l += l;
                rev_r += r;
            }
        }
        envelope(v, vreg(i, 4), vreg(i, 5));
        v.vol[0].tick();
        v.vol[1].tick();

        int32_t step = vreg(i, 2);
        if (i > 0 && (pmon >> i & 1)) {
            int32_t factor = voices_[static_cast<std::size_t>(i) - 1].out + 0x8000;
            step = (static_cast<int16_t>(step) * factor) >> 15;
            step &= 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        v.counter += static_cast<uint32_t>(step);
        if ((v.counter >> 12) >= 28) {
            v.counter -= 28u << 12;
            next_block(v, i);
        }
    }

    // CD / XA input (captured before the CD volume).
    int16_t cd_l = 0, cd_r = 0;
    if (cd_) cd_->next_frame(cd_l, cd_r);
    {
        const uint32_t pos = capture_pos_ * 2;
        auto put = [&](uint32_t base, int16_t v) {
            ram_[base + pos] = static_cast<uint8_t>(v);
            ram_[base + pos + 1] = static_cast<uint8_t>(v >> 8);
        };
        put(0x000, cd_l);
        put(0x400, cd_r);
        put(0x800, voices_[1].out);
        put(0xC00, voices_[3].out);
        if (regs_[kTransferCtrl >> 1] & 0xC)
            for (uint32_t base : {0x000u, 0x400u, 0x800u, 0xC00u}) check_irq_range(base + pos, 2);
        capture_pos_ = (capture_pos_ + 1) & 0x1FF;
    }
    int32_t cdl = 0, cdr = 0;
    if (cnt & 1) {
        cdl = (cd_l * static_cast<int16_t>(regs_[kCdVolL >> 1])) >> 15;
        cdr = (cd_r * static_cast<int16_t>(regs_[kCdVolR >> 1])) >> 15;
        const int32_t cd_gain = group_gain_[static_cast<std::size_t>(VoiceGroup::Music)];
        if (cd_gain != 256) {
            cdl = (cdl * cd_gain) >> 8;
            cdr = (cdr * cd_gain) >> 8;
        }
        if (cnt & 4) {
            rev_l += cdl;
            rev_r += cdr;
        }
    }

    int16_t rl = 0, rr = 0;
    reverb_.tick(ram_.data(), &regs_[kReverbRegs >> 1], cnt & 0x80, clamp16(rev_l), clamp16(rev_r), rl, rr);
    sum_l += (rl * static_cast<int16_t>(regs_[kReverbVolL >> 1])) >> 15;
    sum_r += (rr * static_cast<int16_t>(regs_[kReverbVolR >> 1])) >> 15;

    if ((cnt & 0xC000) != 0xC000) sum_l = sum_r = 0;  // SPU disabled or muted (CD input is unaffected)
    int32_t out_l = (clamp16(clamp16(sum_l) + cdl) * main_vol_[0].level) >> 15;
    int32_t out_r = (clamp16(clamp16(sum_r) + cdr) * main_vol_[1].level) >> 15;
    main_vol_[0].tick();
    main_vol_[1].tick();
    audio_.push(clamp16(out_l), clamp16(out_r));
    ++frames_;
}

void Spu::run_to(int64_t now) {
    constexpr int64_t kMaxBehind = 33868800;  // 1 s: skip ahead rather than grind through a huge time jump
    if (now - next_cycle_ > kMaxBehind) next_cycle_ = now - kCyclesPerSample * 8;
    while (next_cycle_ + kCyclesPerSample <= now) {
        generate_sample();
        next_cycle_ += kCyclesPerSample;
    }
}

Scheduler::Callback Spu::batch_callback() {
    return [this](int64_t due) {
        run_to(due);
        schedule_batch(due + kBatchSamples * kCyclesPerSample);
    };
}

void Spu::schedule_batch(int64_t at) { batch_event_ = sched_->schedule(at, batch_callback()); }

void Spu::start(Scheduler& sched) {
    sched_ = &sched;
    next_cycle_ = sched.now();
    schedule_batch(sched.now() + kBatchSamples * kCyclesPerSample);
}

void Spu::io(StateIo& s) {
    StateIo::Section sec(s, "SPU0");
    s.io(ram_);
    s.io(regs_);
    s.io(transfer_addr_);
    s.io(key_on_);
    s.io(endx_);
    for (Voice& v : voices_) {
        s.io(v.addr);
        s.io(v.repeat);
        s.io(v.counter);
        s.io(v.buf);
        s.io(v.s1);
        s.io(v.s2);
        s.io(v.flags);
        s.io(v.phase);
        s.io(v.level);
        s.io(v.adsr_counter);
        s.io(v.out);
        for (Sweep& w : v.vol) {
            s.io(w.reg);
            s.io(w.level);
            s.io(w.counter);
        }
    }
    for (Sweep& w : main_vol_) {
        s.io(w.reg);
        s.io(w.level);
        s.io(w.counter);
    }
    noise_.io(s);
    reverb_.io(s);
    s.io(capture_pos_);
    s.io(next_cycle_);
    s.io(frames_);

    bool batching = !s.loading() && sched_ && sched_->is_pending(batch_event_);
    s.io(batching);
    if (s.loading()) {
        groups_stale_ = true;
        audio_.clear();
        if (batching && !sched_) throw StateError("SPU state has timed generation but start() was not called");
        if (!batching) batch_event_ = kNoEvent;
    }
    if (batching && sched_) sched_->io_event(s, batch_event_, [this] { return batch_callback(); });
}

}  // namespace dw3
