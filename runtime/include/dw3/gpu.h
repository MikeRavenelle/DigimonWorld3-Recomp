#pragma once

// PS1 GPU front end: GP0/GP1 command processing, VRAM transfers, GPUSTAT/GPUREAD, display state and
// DMA channel 2. Drawing primitives are decoded into structured form and handed to a Rasterizer
// (the software reference, or the plume hardware renderer which implements the same interface).

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/precise_vertex.h"
#include "dw3/state.h"
#include "dw3/video_timing.h"

namespace dw3 {

constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;

enum class TexDepth : uint8_t { Bit4 = 0, Bit8 = 1, Bit15 = 2 };

// Drawing environment (E1-E6 + per-primitive texpage/CLUT).
struct DrawState {
    // Texture page (E1 / textured primitives).
    int tex_base_x = 0, tex_base_y = 0;  // in pixels (x multiple of 64, y 0 or 256)
    uint8_t semi_mode = 0;               // 0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4
    TexDepth depth = TexDepth::Bit4;
    bool dither = false;
    bool draw_to_display = false;
    bool texture_disable = false;
    bool rect_flip_x = false, rect_flip_y = false;
    // Texture window (E2), in 8-pixel units as written.
    uint8_t win_mask_x = 0, win_mask_y = 0, win_off_x = 0, win_off_y = 0;
    // Drawing area (E3/E4) inclusive, and offset (E5).
    int area_x1 = 0, area_y1 = 0, area_x2 = 0, area_y2 = 0;
    int off_x = 0, off_y = 0;
    // Mask bits (E6).
    bool set_mask = false, check_mask = false;
};

struct GpuVertex {
    int32_t x = 0, y = 0;   // already offset-adjusted, sign-extended 11-bit
    uint8_t r = 0, g = 0, b = 0;
    uint8_t u = 0, v = 0;
    // Sub-pixel position and depth from the GTE (see precise_vertex.h), in the same offset-adjusted pixel
    // coordinates as x and y; only meaningful when `precise` is set. Renderers other than the GPU one ignore it.
    float fx = 0, fy = 0, w = 0;
    bool precise = false;
};

struct PrimitiveFlags {
    bool textured = false;
    bool raw_texture = false;  // texture colour not modulated by vertex colour
    bool semi_transparent = false;
    bool gouraud = false;
    uint16_t clut = 0;  // CLUT attribute word (x/16 in bits 0-5, y in bits 6-14)
};

// Rectangle in VRAM pixels. x/y may be any value (taken modulo the VRAM size); w/h are 1..1024 / 1..512.
struct VramRect {
    int x = 0, y = 0, w = kVramWidth, h = kVramHeight;
};

class Rasterizer {
public:
    virtual ~Rasterizer() = default;
    // Coherency hooks for renderers that draw into a copy of VRAM elsewhere (the GPU renderer). The Gpu calls
    // vram_read_begin before it reads the CPU-side VRAM array outside of a draw call (transfers, display
    // snapshots), and vram_written after it modified a region of that array directly. The software rasterizer
    // draws straight into the array and ignores both.
    virtual void vram_read_begin(std::span<uint16_t> vram, const VramRect& r) {}
    virtual void vram_written(std::span<const uint16_t> vram, const VramRect& r) {}
    // VRAM fills and VRAM-to-VRAM copies done by the renderer itself. A renderer that returns true has performed
    // the operation on its copy; the CPU-side array is not touched and is refreshed through vram_read_begin when
    // it is next read. Returning false (the default) makes the Gpu do the operation on the array.
    virtual bool vram_fill(const VramRect& r, uint16_t color) { return false; }
    virtual bool vram_copy(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) { return false; }
    virtual void triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f,
                          const GpuVertex v[3]) = 0;
    // Axis-aligned rectangle/sprite; v is the top-left vertex (uv = top-left texcoord).
    virtual void rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v,
                      int width, int height) = 0;
    virtual void line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
                      const GpuVertex& b) = 0;
    // The game pointed the display at another frame buffer (GP1 05): the frame it drew is finished.
    virtual void display_flipped() {}
};

// Display configuration from GP1 (what the TV shows).
struct DisplayState {
    bool enabled = false;
    bool color24 = false;
    bool interlaced = false;
    bool pal = false;
    int vram_x = 0, vram_y = 0;           // GP1(05)
    int h_start = 0x200, h_end = 0xC00;   // GP1(06), video clocks
    int v_start = 0x10, v_end = 0x100;    // GP1(07), scanlines
    int width = 320, height = 240;        // from GP1(08)
    // Shown at this aspect; the renderer widens it from 4:3 when it substitutes its wide frame copy.
    int aspect_num = 4, aspect_den = 3;
};

class Gpu : public DmaDevice {
public:
    static constexpr uint32_t kGp0 = 0x1F801810u;
    static constexpr uint32_t kGp1 = 0x1F801814u;

    Gpu(Bus& bus, IrqController& irq, VideoTiming& video, Dma& dma);

    void set_rasterizer(Rasterizer* r) { raster_ = r; }
    Rasterizer* rasterizer() const { return raster_; }
    // Source of sub-pixel vertex positions for polygon vertices (null: integer vertices only).
    void set_precise_source(const PreciseVertexCache* c) { precise_ = c; }

    void write_gp0(uint32_t word);
    void write_gp1(uint32_t word);
    uint32_t read_gpustat() const;
    uint32_t read_gpuread();

    // DmaDevice (channel 2).
    void dma_write(std::span<const uint32_t> words) override;
    void dma_read(std::span<uint32_t> out) override;

    std::span<uint16_t> vram() { return vram_; }
    std::span<const uint16_t> vram() const { return vram_; }
    const DisplayState& display() const { return display_; }
    const DrawState& draw_state() const { return draw_; }

    // Converts the displayed VRAM region to RGBA8888 (opaque). Returns false if display is disabled.
    // Not const: a hardware renderer first syncs the region it drew back into VRAM.
    bool snapshot_display(std::vector<uint32_t>& rgba, int& width, int& height);
    // Whole VRAM as RGBA8888 (15-bit interpretation), for debugging/audit dumps.
    void snapshot_vram(std::vector<uint32_t>& rgba);

    // Diagnostics.
    uint64_t commands() const { return commands_; }
    uint64_t primitives() const { return primitives_; }
    std::function<void(const char*, uint32_t)> on_unknown_command;

    // Save state: VRAM, command state machine, environment, display and transfer state. With a rasterizer set,
    // saving first pulls its copy of VRAM into the array and loading pushes the array back to it.
    void io(StateIo& s);

private:
    enum class Gp0State { Command, ImageLoad, PolyLine };

    void reset();
    void execute(std::span<const uint32_t> cmd);
    void execute_env(uint32_t word);
    void draw_polygon(std::span<const uint32_t> cmd);
    void draw_rect(std::span<const uint32_t> cmd);
    void draw_line(const GpuVertex& a, const GpuVertex& b, const PrimitiveFlags& f);
    void fill_rect(std::span<const uint32_t> cmd);
    void copy_vram(std::span<const uint32_t> cmd);
    void begin_image_load(std::span<const uint32_t> cmd);
    void image_load_word(uint32_t w);
    void begin_image_store(std::span<const uint32_t> cmd);
    void apply_texpage(uint32_t attr);
    GpuVertex make_vertex(uint32_t xy) const;
    void store_pixel(int x, int y, uint16_t c);
    int expected_length(uint32_t first) const;
    void update_video_mode();

    Bus& bus_;
    IrqController& irq_;
    VideoTiming& video_;
    Rasterizer* raster_ = nullptr;
    const PreciseVertexCache* precise_ = nullptr;

    std::vector<uint16_t> vram_;
    DrawState draw_;
    DisplayState display_;

    Gp0State state_ = Gp0State::Command;
    std::vector<uint32_t> fifo_;
    int needed_ = 0;

    // Image load/store (CPU<->VRAM) rectangle and cursor.
    int xfer_x = 0, xfer_y = 0, xfer_w = 0, xfer_h = 0, xfer_cx = 0, xfer_cy = 0;
    bool store_active_ = false;

    // Poly-line state.
    PrimitiveFlags line_flags_;
    GpuVertex line_last_;
    uint32_t line_color_ = 0;
    bool line_expect_color_ = false;

    uint32_t gpuread_ = 0;
    uint32_t stat_ = 0;          // GPUSTAT bits stored directly (texpage mirror, mode, dma dir, irq...)
    uint8_t dma_dir_ = 0;
    bool tex_disable_allowed_ = false;  // GP1(09)
    uint64_t commands_ = 0, primitives_ = 0;
};

}  // namespace dw3
