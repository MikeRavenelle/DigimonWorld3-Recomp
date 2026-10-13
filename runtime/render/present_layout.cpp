#include "dw3/present_layout.h"

#include <algorithm>
#include <cstring>

namespace dw3 {

PresentRect compute_present_rect(int src_w, int src_h, int win_w, int win_h, ScaleMode mode, int aspect_num,
                                 int aspect_den) {
    if (src_w <= 0 || src_h <= 0 || win_w <= 0 || win_h <= 0) return {};
    if (mode == ScaleMode::Stretch) return {0, 0, win_w, win_h};

    auto centred = [&](int w, int h) { return PresentRect{(win_w - w) / 2, (win_h - h) / 2, w, h}; };
    auto fit = [&]() {
        // Compare win_w / win_h against the aspect without floating point.
        const int64_t lhs = static_cast<int64_t>(win_w) * aspect_den;
        const int64_t rhs = static_cast<int64_t>(win_h) * aspect_num;
        int w, h;
        if (lhs <= rhs) {  // window is narrower than the aspect: width-limited
            w = win_w;
            h = static_cast<int>(static_cast<int64_t>(win_w) * aspect_den / aspect_num);
        } else {
            h = win_h;
            w = static_cast<int>(static_cast<int64_t>(win_h) * aspect_num / aspect_den);
        }
        return centred(std::max(w, 1), std::max(h, 1));
    };

    if (mode == ScaleMode::Integer) {
        // Aspect-corrected size of one display pixel row set: height src_h, width src_h * aspect.
        const int64_t base_w = static_cast<int64_t>(src_h) * aspect_num / aspect_den;
        const int64_t scale = std::min<int64_t>(win_w / base_w, win_h / static_cast<int64_t>(src_h));
        if (scale >= 1) return centred(static_cast<int>(base_w * scale), static_cast<int>(src_h * scale));
    }
    return fit();
}

bool parse_scale_mode(const char* text, ScaleMode& out) {
    if (!text) return false;
    if (!std::strcmp(text, "fit")) out = ScaleMode::Fit;
    else if (!std::strcmp(text, "integer")) out = ScaleMode::Integer;
    else if (!std::strcmp(text, "stretch")) out = ScaleMode::Stretch;
    else return false;
    return true;
}

}  // namespace dw3
