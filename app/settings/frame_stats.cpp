#include "frame_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dw3app {

int64_t FrameStatSeries::percentile(double fraction) const {
    if (samples_.empty()) return 0;
    std::vector<int64_t> sorted = samples_;
    const std::size_t rank = static_cast<std::size_t>(std::ceil(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(sorted.size())));
    const std::size_t index = rank == 0 ? 0 : rank - 1;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), sorted.end());
    return sorted[index];
}

int64_t FrameStatSeries::max() const { return samples_.empty() ? 0 : *std::max_element(samples_.begin(), samples_.end()); }

std::size_t FrameStatSeries::max_index() const {
    return samples_.empty() ? 0 : static_cast<std::size_t>(std::max_element(samples_.begin(), samples_.end()) - samples_.begin());
}

std::size_t FrameStatSeries::count_over(int64_t nanoseconds) const {
    return static_cast<std::size_t>(std::count_if(samples_.begin(), samples_.end(), [&](int64_t v) { return v > nanoseconds; }));
}

FrameStatSeries& FrameStats::series(const std::string& name) {
    for (auto& [existing, data] : series_)
        if (existing == name) return data;
    series_.emplace_back(name, FrameStatSeries{});
    return series_.back().second;
}

std::string FrameStats::report() const {
    std::string text;
    char line[200];
    for (const auto& [name, data] : series_) {
        std::snprintf(line, sizeof(line),
                      "  frame-stats %-8s n=%zu p50 %.2f p95 %.2f p99 %.2f max %.2f ms (frame %zu), over 16.7 ms: %zu, over 33 ms: %zu\n",
                      name.c_str(), data.count(), static_cast<double>(data.percentile(0.50)) / 1e6,
                      static_cast<double>(data.percentile(0.95)) / 1e6, static_cast<double>(data.percentile(0.99)) / 1e6,
                      static_cast<double>(data.max()) / 1e6, data.max_index(), data.count_over(kFrameBudget60Ns), data.count_over(kFrameBudget30Ns));
        text += line;
    }
    return text;
}

}  // namespace dw3app
