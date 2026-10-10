#include "dw3/video_timing.h"

namespace dw3 {

VideoTiming::VideoTiming(Scheduler& sched, IrqController& irq) : sched_(sched), irq_(irq) {
    origin_ = sched_.now();
    schedule_vblank();
}

namespace {
// floor(a * num / den) without intermediate overflow for long sessions.
int64_t mul_div_floor(int64_t a, int64_t num, int64_t den) { return (a / den) * num + (a % den) * num / den; }
}  // namespace

int64_t VideoTiming::to_video_clocks(int64_t cpu_cycles) const { return mul_div_floor(cpu_cycles, clk_num(), kClkDen); }

int64_t VideoTiming::to_cpu_cycles_ceil(int64_t vc) const {
    return mul_div_floor(vc, kClkDen, clk_num()) + ((vc % clk_num()) * kClkDen % clk_num() != 0 ? 1 : 0);
}

int64_t VideoTiming::vc_of(int64_t cycle) const { return cycle <= origin_ ? 0 : to_video_clocks(cycle - origin_); }

int64_t VideoTiming::cycle_of(int64_t vc) const { return origin_ + to_cpu_cycles_ceil(vc); }

// A field is lines_total/den lines: 263 (NTSC) or 314 (PAL) when progressive, half of 525 / 625 when interlaced.
int64_t VideoTiming::field_start_vc(uint64_t k) const {
    int64_t total = standard_ == VideoStandard::Ntsc ? 525 : 625;
    int64_t num = interlaced_ ? total : (standard_ == VideoStandard::Ntsc ? 263 : 314);
    int64_t den = interlaced_ ? 2 : 1;
    return static_cast<int64_t>(k) * num * line_clocks() / den;
}

uint64_t VideoTiming::field_of(int64_t vc) const {
    int64_t total = standard_ == VideoStandard::Ntsc ? 525 : 625;
    int64_t num = (interlaced_ ? total : (standard_ == VideoStandard::Ntsc ? 263 : 314)) * line_clocks();
    int64_t den = interlaced_ ? 2 : 1;
    uint64_t k = static_cast<uint64_t>(vc * den / num);
    while (field_start_vc(k + 1) <= vc) ++k;
    while (k > 0 && field_start_vc(k) > vc) --k;
    return k;
}

int64_t VideoTiming::vblank_cycle(uint64_t k) const {
    return cycle_of(field_start_vc(k) + static_cast<int64_t>(vblank_line()) * line_clocks());
}

bool VideoTiming::in_hblank(int64_t cycle) const { return vc_of(cycle) % line_clocks() >= kHblankStartClk; }

bool VideoTiming::in_vblank(int64_t cycle) const {
    int64_t vc = vc_of(cycle);
    return vc >= field_start_vc(field_of(vc)) + static_cast<int64_t>(vblank_line()) * line_clocks();
}

int64_t VideoTiming::next_hblank_edge(int64_t cycle) const {
    int64_t vc = vc_of(cycle), l = line_clocks(), pos = vc % l;
    return cycle_of(vc - pos + (pos < kHblankStartClk ? kHblankStartClk : l));
}

int64_t VideoTiming::next_vblank_edge(int64_t cycle) const {
    int64_t vc = vc_of(cycle);
    uint64_t k = field_of(vc);
    int64_t vb = field_start_vc(k) + static_cast<int64_t>(vblank_line()) * line_clocks();
    return cycle_of(vc >= vb ? field_start_vc(k + 1) : vb);
}

uint64_t VideoTiming::hblank_count(int64_t cycle) const {
    int64_t vc = vc_of(cycle);
    uint64_t n = vc < kHblankStartClk ? 0 : static_cast<uint64_t>((vc - kHblankStartClk) / line_clocks() + 1);
    return hblank_base_ + n;
}

int VideoTiming::scanline(int64_t cycle) const {
    int64_t vc = vc_of(cycle);
    return static_cast<int>((vc - field_start_vc(field_of(vc))) / line_clocks());
}

void VideoTiming::schedule_vblank() {
    vblank_event_ = sched_.schedule(vblank_cycle(next_k_), [this](int64_t) {
        ++next_k_;
        ++frame_count_;
        irq_.raise(IrqSource::Vblank);
        schedule_vblank();  // absolute time from the frame index: never drifts
        if (on_frame) on_frame(frame_count_);
    });
}

template <class F>
void VideoTiming::reconfigure(F&& apply, bool restart) {
    if (on_before_reconfigure) on_before_reconfigure();
    if (restart) {
        sched_.cancel(vblank_event_);
        hblank_base_ = hblank_count(sched_.now());
    }
    apply();
    if (restart) {
        origin_ = sched_.now();
        next_k_ = 0;
        schedule_vblank();
    }
    if (on_after_reconfigure) on_after_reconfigure();
}

void VideoTiming::set_standard(VideoStandard s) {
    if (s != standard_) reconfigure([&] { standard_ = s; }, true);
}

void VideoTiming::set_interlaced(bool on) {
    if (on != interlaced_) reconfigure([&] { interlaced_ = on; }, true);
}

void VideoTiming::set_dot_divider(int divider) {
    if (divider > 0 && divider != dot_divider_) reconfigure([&] { dot_divider_ = divider; }, false);
}

}  // namespace dw3
