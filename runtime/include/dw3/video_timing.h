#pragma once

// Frame / scanline timing in CPU cycles. The GPU video clock is 53.693175 MHz (NTSC) / 53.203425 MHz (PAL)
// against the 33.8688 MHz CPU clock (ratios 715909/451584 and 709379/451584); a scanline is 3413 video
// clocks (NTSC) or 3406 (PAL). Everything is a pure function of ctx.cycles (no per-line events), so
// the root counters can evaluate HBLANK/VBLANK state lazily; only the VBLANK start is scheduled.

#include <cstdint>
#include <functional>

#include "dw3/irq.h"
#include "dw3/scheduler.h"

namespace dw3 {

enum class VideoStandard { Ntsc, Pal };

class VideoTiming {
public:
    static constexpr int64_t kCpuHz = 33868800;
    static constexpr int64_t kHblankStartClk = 2560;  // hblank spans [2560, line_clocks) of each line

    VideoTiming(Scheduler& sched, IrqController& irq);

    // Reconfiguration restarts the frame position at the current cycle (frame_count keeps counting).
    void set_standard(VideoStandard s);
    void set_interlaced(bool on);
    void set_dot_divider(int divider);  // video clocks per dot: 10 (256), 8 (320), 7 (368), 5 (512), 4 (640)

    VideoStandard standard() const { return standard_; }
    bool interlaced() const { return interlaced_; }
    int dot_divider() const { return dot_divider_; }
    // CPU cycles <-> video clocks for the current standard (exact ratio, floor / ceil).
    int64_t to_video_clocks(int64_t cpu_cycles) const;
    int64_t to_cpu_cycles_ceil(int64_t video_clocks) const;
    int line_clocks() const { return standard_ == VideoStandard::Ntsc ? 3413 : 3406; }
    // Lines in a full frame: 263 / 314 progressive, 525 / 625 interlaced (two fields).
    int lines_per_frame() const {
        return standard_ == VideoStandard::Ntsc ? (interlaced_ ? 525 : 263) : (interlaced_ ? 625 : 314);
    }
    int vblank_line() const { return standard_ == VideoStandard::Ntsc ? 240 : 288; }

    uint64_t frame_count() const { return frame_count_; }

    // Cycle at which the VBLANK of frame/field `k` (0-based since the last reconfigure) starts.
    int64_t vblank_cycle(uint64_t k) const;

    // State queries, valid for any cycle >= the last reconfigure point.
    bool in_hblank(int64_t cycle) const;
    bool in_vblank(int64_t cycle) const;
    int64_t next_hblank_edge(int64_t cycle) const;  // first cycle > `cycle` where in_hblank() changes
    int64_t next_vblank_edge(int64_t cycle) const;
    uint64_t hblank_count(int64_t cycle) const;     // HBLANK starts up to and including `cycle`, since construction
    int scanline(int64_t cycle) const;              // 0-based line within the current frame/field

    // Invoked at each VBLANK start after the IRQ is raised (the app presents the frame here).
    std::function<void(uint64_t frame)> on_frame;
    // Invoked around any change of timing parameters so lazily-evaluated consumers (timers) can sync.
    std::function<void()> on_before_reconfigure, on_after_reconfigure;

    // Save state: timing parameters, the frame origin and the pending VBLANK event (which fires at the same cycle
    // after loading). on_frame is not called.
    void io(StateIo& s);

private:
    template <class F>
    void reconfigure(F&& apply, bool restart);
    int64_t vc_of(int64_t cycle) const;
    // Video clocks per CPU cycle, as an exact fraction.
    int64_t clk_num() const { return standard_ == VideoStandard::Ntsc ? 715909 : 709379; }
    static constexpr int64_t kClkDen = 451584;
    int64_t cycle_of(int64_t vc) const;  // first CPU cycle at or after video clock `vc`
    int64_t field_start_vc(uint64_t k) const;
    uint64_t field_of(int64_t vc) const;
    void schedule_vblank();
    void on_vblank();

    Scheduler& sched_;
    IrqController& irq_;
    VideoStandard standard_ = VideoStandard::Ntsc;
    bool interlaced_ = false;
    int dot_divider_ = 8;
    int64_t origin_ = 0;
    uint64_t hblank_base_ = 0;
    uint64_t next_k_ = 0, frame_count_ = 0;
    EventId vblank_event_ = kNoEvent;
};

}  // namespace dw3
