// On-screen notifications drawn by the presenters over the final window image. The text is rasterized on the CPU
// into a small RGBA8 image with an embedded 8x8 bitmap font; nothing here touches the emulated VRAM or display.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dw3app {

// RGBA8 pixels, one uint32_t each with the red channel in the lowest byte (SDL ABGR8888 / Vulkan R8G8B8A8).
struct OsdImage {
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
};

constexpr int kOsdGlyph = 8;           // glyph cell size in font pixels
constexpr uint32_t kOsdTextColor = 0xFFF0F0F0u;
constexpr uint32_t kOsdBoxColor = 0xBE241810u;  // translucent dark blue-grey

// Font pixels per glyph pixel for a window of the given height: integer, never below 2.
int osd_scale(int window_height);

// Padding between the box edge and the text, in output pixels.
int osd_padding(int scale);

// Size of the box for `chars` characters.
void osd_box_size(int chars, int scale, int& w, int& h);

// Rasterizes `text` on one line inside a box. Characters outside printable ASCII are drawn as '?'. Text that does
// not fit in `max_width` pixels is cut at the last whole character that does. Returns an empty image if not even
// one character fits.
OsdImage osd_render_text(std::string_view text, int scale, int max_width);

// The current notification and its lifetime. Time is whatever monotonic millisecond clock the caller uses.
class Osd {
public:
    static constexpr uint64_t kShowMs = 2000;
    static constexpr uint64_t kFadeMs = 400;

    void show(std::string text, uint64_t now_ms);
    // The image to draw this frame, or null when there is nothing to show. `opacity` receives 0..1. The image is
    // rebuilt only when the text or the window size changed; `serial()` changes whenever the pixels do.
    const OsdImage* frame(int window_w, int window_h, uint64_t now_ms, float& opacity);
    uint64_t serial() const { return serial_; }
    // Window position of the box's top-left corner for the last frame().
    int margin() const { return margin_; }

private:
    std::string text_;
    uint64_t shown_at_ = 0;
    bool active_ = false;
    OsdImage image_;
    std::string image_text_;
    int image_scale_ = 0, image_width_ = 0;
    int margin_ = 0;
    uint64_t serial_ = 0;
};

}  // namespace dw3app
