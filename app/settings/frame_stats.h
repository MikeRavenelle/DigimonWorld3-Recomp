#pragma once

// Per-frame timing samples for diagnosing stutter: percentiles and counts of frames over the 60 Hz and 30 Hz budgets.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dw3app {

constexpr int64_t kFrameBudget60Ns = 16'700'000;
constexpr int64_t kFrameBudget30Ns = 33'000'000;

class FrameStatSeries {
public:
    void add(int64_t nanoseconds) { samples_.push_back(nanoseconds); }
    std::size_t count() const { return samples_.size(); }
    // Nearest-rank percentile, `fraction` in [0, 1]; 0 for an empty series.
    int64_t percentile(double fraction) const;
    int64_t max() const;
    // Position of the largest sample (the first one on a tie); 0 for an empty series.
    std::size_t max_index() const;
    std::size_t count_over(int64_t nanoseconds) const;

private:
    std::vector<int64_t> samples_;
};

// Named series of one run, summarised as text lines.
class FrameStats {
public:
    FrameStatSeries& series(const std::string& name);
    std::string report() const;

private:
    std::vector<std::pair<std::string, FrameStatSeries>> series_;
};

}  // namespace dw3app
