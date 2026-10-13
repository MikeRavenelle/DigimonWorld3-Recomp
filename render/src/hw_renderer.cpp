// GPU renderer on plume. See hw_renderer.h for the overall design.

#include "dw3/hw_renderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "dw3/hdr.h"
#include "dw3/texrep/blocks.h"
#include "dw3/texrep/dump.h"
#include "dw3/texrep/pack.h"
#include "dw3/vertex_builder.h"
#include "dw3/vram_regions.h"
#include "dw3/widescreen.h"
#include "plume_render_interface.h"
#include "post_chain.h"
#ifdef _WIN32
// <windows.h> (pulled in by the Direct3D 12 headers) renames DrawState to DrawStateA.
#undef DrawState
#endif
#include "shaders/osd.frag.h"
#include "shaders/osd.vert.h"
#include "shaders/present.frag.h"
#include "shaders/present.vert.h"
#include "shaders/psx_draw.frag.h"
#include "shaders/psx_draw_layer.frag.h"
#include "shaders/psx_draw.vert.h"
#include "shaders/vram_copy.frag.h"
#include "shaders/vram_copy.vert.h"
#ifdef DW3_HAVE_METAL
#include "shaders/osd.frag.msl.h"
#include "shaders/osd.vert.msl.h"
#include "shaders/present.frag.msl.h"
#include "shaders/present.vert.msl.h"
#include "shaders/psx_draw.frag.msl.h"
#include "shaders/psx_draw_layer.frag.msl.h"
#include "shaders/psx_draw.vert.msl.h"
#include "shaders/vram_copy.frag.msl.h"
#include "shaders/vram_copy.vert.msl.h"
#endif
#ifdef DW3_HAVE_D3D12
#include "shaders/osd.frag.dxil.h"
#include "shaders/osd.vert.dxil.h"
#include "shaders/present.frag.dxil.h"
#include "shaders/present.vert.dxil.h"
#include "shaders/psx_draw.frag.dxil.h"
#include "shaders/psx_draw_layer.frag.dxil.h"
#include "shaders/psx_draw.vert.dxil.h"
#include "shaders/vram_copy.frag.dxil.h"
#include "shaders/vram_copy.vert.dxil.h"
#endif

namespace plume {
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#ifdef DW3_HAVE_METAL
extern std::unique_ptr<RenderInterface> CreateMetalInterface();
#endif
#ifdef DW3_HAVE_D3D12
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern void SetVulkanExtraInstanceExtensions(const std::vector<std::string>& extensions);  // from the plume patch
}

namespace dw3 {

using namespace plume;

namespace {

constexpr RenderFormat kVramFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr RenderFormat kLayerFormat = RenderFormat::R16G16B16A16_UNORM;
constexpr RenderFormat kSwapFormat = RenderFormat::B8G8R8A8_UNORM;

constexpr std::size_t kVertexBufferBytes = 48u << 20;
constexpr std::size_t kStagingBytes = 16u << 20;
constexpr std::size_t kReadbackBytes = 8u << 20;
constexpr std::size_t kMaxPendingVertices = 200000;
// Frames the CPU may run ahead of the GPU: each slot has its own command buffer, fence and upload buffers.
constexpr int kFramesInFlight = 2;

struct PresentParams {
    float dest[4];
    float window[2];
    float pad0[2];
    int32_t src[4];
    int32_t color24;
    int32_t linear;
    int32_t scale;
    int32_t layer;  // show the display layer where it is valid
};
static_assert(sizeof(PresentParams) == 64);

// Push constants of the notification pass (see osd.vert).
struct OsdParams {
    float dest[4];
    float window[2];
    float opacity;
    float hdr_scale;  // paper white / 80 nits on a scRGB swap chain, otherwise 0
};
static_assert(sizeof(OsdParams) == 32);

// Push constants of the VRAM copy/fill pass (see vram_copy.frag).
struct CopyParams {
    int32_t rect[4];  // source x, y, destination x, y
    uint32_t flags;   // 1 fill, 2 check mask, 4 set mask
    uint32_t color;
    int32_t scale;
    int32_t pad;
};
static_assert(sizeof(CopyParams) == 32);

// 5-bit channel -> 8-bit expansion used to store VRAM words in an RGBA8 texture.
std::array<uint8_t, 32> make_expand_table() {
    std::array<uint8_t, 32> t{};
    for (unsigned i = 0; i < 32; ++i) t[i] = static_cast<uint8_t>((i << 3) | (i >> 2));
    return t;
}
const std::array<uint8_t, 32> kExpand = make_expand_table();

inline uint32_t word_to_rgba(uint16_t w) {
    return kExpand[w & 31] | (static_cast<uint32_t>(kExpand[(w >> 5) & 31]) << 8) |
           (static_cast<uint32_t>(kExpand[(w >> 10) & 31]) << 16) | ((w & 0x8000) ? 0xFF000000u : 0u);
}

inline uint16_t rgba_to_word(uint32_t p) {
    return static_cast<uint16_t>(((p & 0xFF) >> 3) | ((((p >> 8) & 0xFF) >> 3) << 5) | ((((p >> 16) & 0xFF) >> 3) << 10) |
                                 ((p >> 31) << 15));
}

std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

}  // namespace

struct HwRenderer::Impl {
    struct DrawRange {
        uint32_t first = 0, count = 0;
        Region scissor;
    };

    std::unique_ptr<RenderInterface> iface;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    // One slot per frame in flight. `cmd`, the buffers and their mappings point into the current slot.
    struct Slot {
        std::unique_ptr<RenderCommandList> cmd;
        std::unique_ptr<RenderCommandFence> fence;
        std::unique_ptr<RenderBuffer> vertex_buf, staging_buf;
        std::unique_ptr<RenderCommandSemaphore> acquire_sem;
        uint8_t* vertex_map = nullptr;
        uint8_t* staging_map = nullptr;
        bool pending = false;  // submitted, fence not yet waited on
    };
    std::array<Slot, kFramesInFlight> slots;
    int cur_slot = 0, last_submitted = 0;
    bool slot_ready = false;  // the current slot's previous work has been waited for
    RenderCommandList* cmd = nullptr;
    RenderBuffer* vertex_buf = nullptr;
    RenderBuffer* staging_buf = nullptr;
    bool recording = false;
    HwStats stats;

    // Internal resolution multiplier: `rt` and `tex` are (1024 * scale) x (512 * scale).
    int scale = 1;
    // Display layer (see hw_renderer.h): `hi` is drawn next to `rt`, `hi_tex` is its sampled copy.
    bool layer = false;
    bool precise = false;  // sub-pixel triangles (HwOptions::precise_geometry)

    // Texture replacement (see texrep/blocks.h). `pack` is set while replacements are active; the atlas holds the
    // replacement images that sprites currently refer to.
    static constexpr int kAtlasSize = 4096;
    std::shared_ptr<const texrep::TexturePack> pack;
    texrep::BlockRegistry blocks;
    std::unique_ptr<texrep::TextureDump> dump;
    std::string dump_dir;
    std::unique_ptr<RenderTexture> atlas;
    RenderTextureLayout atlas_layout = RenderTextureLayout::UNKNOWN;
    texrep::ShelfAtlas atlas_alloc{kAtlasSize, kAtlasSize};
    struct Resident {
        int x = 0, y = 0;
    };
    std::map<std::pair<uint64_t, uint64_t>, Resident> resident;  // (block, palette) -> place in the atlas
    std::set<std::pair<uint64_t, uint64_t>> unusable;            // entries whose image could not be loaded
    int atlas_epoch = 0;  // incremented whenever the atlas is cleared
    bool tracking() const { return pack || dump; }
    // Wide frame copy (see widescreen.h): the targets have extra columns when it is enabled, `mirror` says whether
    // frame-buffer draws are currently duplicated into it, and `wide_ready[i]` that frame buffer i (y = 0, 256)
    // has had its copy drawn since mirroring started.
    WideFrame wide;
    bool mirror = false;
    bool wide_ready[2] = {false, false};
    int target_width() const { return kVramWidth + (wide.enabled() ? wide.width() : 0); }
    bool mirrors(const DrawState& s) const {
        return mirror && wide.in_frame(s.area_x1, s.area_y1, s.area_x2, s.area_y2);
    }
    DisplayState wide_display(const DisplayState& d) const;
    // The post-processing request the swap chain can honour: the HDR output transform is on exactly when the swap chain
    // is HDR (a headless renderer keeps what it is given, for tests).
    const PostFx* effective_post(const PostFx* in, PostFx& storage);
    uint32_t draw_mode = 0;  // push constant of the draw shader: bit0 layer, bit1 true colour, bits 2-3 filter

    // VRAM: `rt` is drawn into; `tex` is the sampled copy. With scale > 1, `nat` is a native-resolution texture
    // that stages CPU uploads (replicated into `rt` on the GPU) and receives the downsampled image for read-backs.
    std::unique_ptr<RenderTexture> rt, tex, nat, hi, hi_tex;
    std::unique_ptr<RenderFramebuffer> rt_fb, nat_fb, draw_fb;  // draw_fb: rt, plus hi when the layer is on
    RenderTextureLayout rt_layout = RenderTextureLayout::UNKNOWN, tex_layout = RenderTextureLayout::UNKNOWN,
                        nat_layout = RenderTextureLayout::UNKNOWN, hi_layout = RenderTextureLayout::UNKNOWN,
                        hi_tex_layout = RenderTextureLayout::UNKNOWN;

    std::unique_ptr<RenderShader> draw_vs, draw_fs, present_vs, present_fs, copy_vs, copy_fs, osd_vs, osd_fs;
    std::unique_ptr<RenderPipelineLayout> draw_layout, present_layout, copy_layout, osd_layout;
    std::unique_ptr<RenderPipeline> draw_pipeline, present_pipeline, copy_pipeline, osd_pipeline;
    std::unique_ptr<RenderDescriptorSet> draw_set, vram_set, present_set, display_set, nat_set, osd_set;
    RenderInputSlot input_slot;
    std::vector<RenderInputElement> input_elements;

    std::unique_ptr<RenderBuffer> readback_buf;
    uint8_t* vertex_map = nullptr;
    uint8_t* staging_map = nullptr;
    std::size_t vertex_used = 0, staging_used = 0;

    // Queued primitives not yet recorded.
    std::vector<HwVertex> pending;
    std::vector<DrawRange> ranges;

    RegionSet dirty;  // drawn on the GPU since the CPU copy was last refreshed
    RegionSet stale;  // changed in `rt` since `tex` was last refreshed

    // Presentation.
    std::unique_ptr<RenderSwapChain> swap;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> release_sems;
    std::vector<std::unique_ptr<RenderFramebuffer>> swap_fbs;
    std::unique_ptr<RenderTexture> capture_tex;
    std::unique_ptr<RenderFramebuffer> capture_fb;
    std::unique_ptr<RenderBuffer> capture_buf;
    RenderTextureLayout capture_layout = RenderTextureLayout::UNKNOWN;
    uint32_t capture_w = 0, capture_h = 0;

    // Post-processing passes, created the first time they are used.
    std::unique_ptr<PostChain> post_chain;

    // The swap chain's format: B8G8R8A8 normally, half floats when HDR output is active.
    RenderFormat swap_format = kSwapFormat;
    bool hdr_wanted = false;
    bool hdr_active = false;
    HdrParams hdr_params;  // the nits of the most recent post-processing request (for the overlay)
    NativeWindow native_window;
    bool has_window = false;

    // Notification image: `osd_tex` holds the last uploaded overlay.
    std::unique_ptr<RenderTexture> osd_tex;
    RenderTextureLayout osd_tex_layout = RenderTextureLayout::UNKNOWN;
    uint32_t osd_w = 0, osd_h = 0;
    uint64_t osd_serial = 0;

    // ---- setup -------------------------------------------------------------------------------------

    bool init(const NativeWindow* window, int internal_scale, const HwOptions& options, std::string& error);
    bool create_targets(std::string& error);
    bool create_pipelines();
    bool create_swapchain(const NativeWindow& window);
    bool recreate_swapchain();
    void create_swap_framebuffers();
    void clear_vram();

    // ---- command recording --------------------------------------------------------------------------

    // Starts recording into the current slot, first waiting until the GPU is done with the frame that last
    // used it (its command buffer and upload buffers). Every write to the slot's buffers comes after this.
    void ensure_recording() {
        if (!recording) {
            if (!slot_ready) {
                wait_slot(slots[cur_slot]);
                slot_ready = true;
            }
            cmd->begin();
            recording = true;
        }
    }
    void wait_slot(Slot& s) {
        if (!s.pending) return;
        queue->waitForCommandFence(s.fence.get());
        s.pending = false;
    }
    void wait_all() {
        for (Slot& s : slots) wait_slot(s);
    }
    void bind_slot() {
        Slot& s = slots[cur_slot];
        cmd = s.cmd.get();
        vertex_buf = s.vertex_buf.get();
        staging_buf = s.staging_buf.get();
        vertex_map = s.vertex_map;
        staging_map = s.staging_map;
        slot_ready = false;
    }
    void transition(RenderTexture* t, RenderTextureLayout& cur, RenderTextureLayout want, bool force = false) {
        if (cur == want && !force) return;
        cmd->barriers(RenderBarrierStage::ALL, RenderTextureBarrier(t, want));
        cur = want;
    }
    void submit(RenderCommandSemaphore* wait = nullptr, RenderCommandSemaphore* signal = nullptr);
    void record_draws();
    void sync_copy();
    void begin_vram_pass(const CopyParams& p);
    void copy_pass(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask);
    void scaled_pass(RenderFramebuffer* fb, RenderDescriptorSet* source, uint32_t flags, const Region& r);
    std::size_t staging_alloc(std::size_t bytes);

    // ---- primitives ---------------------------------------------------------------------------------

    void queue_primitive(const DrawState& s, const Region& bounds, bool reads_dest, const Region* tex_src,
                         const Region* clut_src, std::size_t vertices_before);
    bool draw_background_rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                              int height);
    const Resident* make_resident(const texrep::PackEntry& e);
    void upload_replacement(const texrep::Image& img, int x, int y);
    void upload(std::span<const uint16_t> vram, const Region& r);
    void readback(const std::vector<Region>& regions, std::span<uint16_t> vram);

    // ---- presentation -------------------------------------------------------------------------------

    void draw_triangle(const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3], bool tracked);
    void draw_rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                   int height, bool tracked);
    void draw_line(const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a, const GpuVertex& b, bool tracked);
    DrawState wide_state(const DrawState& s);
    void draw_display(RenderFramebuffer* fb, uint32_t w, uint32_t h, const DisplayState& d, ScaleMode mode, bool linear,
                      const PostChain::Plan* plan = nullptr);
    bool run_post(const PostFx& fx, const DisplayState& d, const PresentRect& pr, PostChain::Plan& plan);
    void push_present(const PresentParams& p);
    bool ensure_capture(uint32_t w, uint32_t h);
    void read_capture(uint32_t w, uint32_t h, std::vector<uint32_t>& out);
    bool upload_overlay(const HwOverlay& o);
    void draw_overlay(uint32_t w, uint32_t h, const HwOverlay& o);
};

// -------------------------------------------------------------------------------------------------------
// Setup

bool HwRenderer::Impl::init(const NativeWindow* window, int internal_scale, const HwOptions& options, std::string& error) {
    scale = std::clamp(internal_scale, 1, 8);
    precise = options.precise_geometry;
    wide = options.wide_frame;
    pack = options.texture_pack;
    layer = options.true_color || options.filter != TextureFilter::Nearest || pack;
    draw_mode = (layer ? 1u : 0u) | (options.true_color ? 2u : 0u) | (static_cast<uint32_t>(options.filter) << 2);
    const char* api_name = "Vulkan";
    if (options.api == GraphicsApi::Metal) {
        api_name = "Metal";
#ifdef DW3_HAVE_METAL
        iface = CreateMetalInterface();
#endif
    } else if (options.api == GraphicsApi::D3D12) {
        api_name = "Direct3D 12";
#ifdef DW3_HAVE_D3D12
        iface = CreateD3D12Interface();
#endif
    } else {
        iface = CreateVulkanInterface();
    }
    if (!iface) {
        error = std::string(api_name) + " is not available";
        return false;
    }
    device = iface->createDevice();
    if (!device) {
        error = std::string("no usable ") + api_name + " device";
        return false;
    }
    queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    if (!queue) {
        error = std::string(api_name) + " queue creation failed";
        return false;
    }
    for (Slot& sl : slots) {
        sl.cmd = queue->createCommandList();
        sl.fence = device->createCommandFence();
        sl.vertex_buf = device->createBuffer(RenderBufferDesc::VertexBuffer(kVertexBufferBytes, RenderHeapType::UPLOAD));
        sl.staging_buf = device->createBuffer(RenderBufferDesc::UploadBuffer(kStagingBytes));
        if (!sl.cmd || !sl.fence || !sl.vertex_buf || !sl.staging_buf) {
            error = std::string(api_name) + " resource creation failed";
            return false;
        }
        sl.vertex_map = static_cast<uint8_t*>(sl.vertex_buf->map());
        sl.staging_map = static_cast<uint8_t*>(sl.staging_buf->map());
        if (!sl.vertex_map || !sl.staging_map) {
            error = "buffer mapping failed";
            return false;
        }
    }
    bind_slot();

    if (!create_targets(error)) return false;

    readback_buf = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kReadbackBytes));
    if (!readback_buf) {
        error = "buffer creation failed";
        return false;
    }

    hdr_wanted = options.hdr;
    if (window) {
        native_window = *window;
        has_window = true;
        if (!create_swapchain(*window)) {
            error = "swap chain creation failed";
            return false;
        }
    }
    if (!create_pipelines()) {
        error = "pipeline creation failed";
        return false;
    }
    clear_vram();
    pending.reserve(kMaxPendingVertices + 8192);
    return true;
}

bool HwRenderer::Impl::create_targets(std::string& error) {
    rt.reset();
    tex.reset();
    nat.reset();
    hi.reset();
    hi_tex.reset();
    rt_fb.reset();
    nat_fb.reset();
    draw_fb.reset();
    atlas.reset();
    atlas_layout = RenderTextureLayout::UNKNOWN;
    resident.clear();
    unusable.clear();
    atlas_alloc.reset();
    ++atlas_epoch;
    rt_layout = tex_layout = nat_layout = hi_layout = hi_tex_layout = RenderTextureLayout::UNKNOWN;
    rt = device->createTexture(RenderTextureDesc::ColorTarget(target_width() * scale, 512 * scale, kVramFormat));
    tex = device->createTexture(RenderTextureDesc::Texture2D(target_width() * scale, 512 * scale, 1, kVramFormat));
    if (scale > 1) nat = device->createTexture(RenderTextureDesc::ColorTarget(1024, 512, kVramFormat));
    if (!rt || !tex || (scale > 1 && !nat)) {
        error = "VRAM texture creation failed";
        return false;
    }
    const RenderTexture* attachment = rt.get();
    RenderFramebufferDesc fbd;
    fbd.colorAttachments = &attachment;
    fbd.colorAttachmentsCount = 1;
    rt_fb = device->createFramebuffer(fbd);
    if (layer) {
        hi = device->createTexture(RenderTextureDesc::ColorTarget(target_width() * scale, 512 * scale, kLayerFormat));
        hi_tex = device->createTexture(RenderTextureDesc::Texture2D(target_width() * scale, 512 * scale, 1, kLayerFormat));
        if (!hi || !hi_tex) {
            error = "display layer creation failed";
            return false;
        }
        // The replacement atlas is only as large as it needs to be: a dummy texel unless a pack is active.
        const int atlas_px = pack ? kAtlasSize : 1;
        atlas = device->createTexture(RenderTextureDesc::Texture2D(atlas_px, atlas_px, 1, kVramFormat));
        if (!atlas) {
            error = "replacement atlas creation failed";
            return false;
        }
        const RenderTexture* both[2] = {rt.get(), hi.get()};
        RenderFramebufferDesc dfd;
        dfd.colorAttachments = both;
        dfd.colorAttachmentsCount = 2;
        draw_fb = device->createFramebuffer(dfd);
        if (!draw_fb) {
            error = "framebuffer creation failed";
            return false;
        }
    }
    if (nat) {
        const RenderTexture* nat_attachment = nat.get();
        fbd.colorAttachments = &nat_attachment;
        nat_fb = device->createFramebuffer(fbd);
    }
    if (!rt_fb || (nat && !nat_fb)) {
        error = "framebuffer creation failed";
        return false;
    }
    return true;
}

bool HwRenderer::Impl::create_pipelines() {
    const RenderShaderFormat fmt = iface->getCapabilities().shaderFormat;
    // Each shader is embedded as SPIR-V (Vulkan) and, in builds that contain those backends, as DXIL (Direct3D 12) and
    // as Metal Shading Language source (Metal).
    struct Blob {
        const uint8_t* data;
        size_t size;
    };
    auto make = [&](Blob spv, Blob dxil, Blob msl) -> std::unique_ptr<RenderShader> {
        if (fmt == RenderShaderFormat::SPIRV) return device->createShader(spv.data, spv.size, "main", fmt);
        if (fmt == RenderShaderFormat::DXIL && dxil.data) return device->createShader(dxil.data, dxil.size, "main", fmt);
        // SPIRV-Cross names the entry point main0, since main is reserved in Metal.
        if (fmt == RenderShaderFormat::METAL && msl.data) return device->createShader(msl.data, msl.size, "main0", fmt);
        return nullptr;
    };
#ifdef DW3_HAVE_D3D12
#define DW3_DXIL(name) Blob{shaders::name##_dxil, shaders::name##_dxil_size}
#else
#define DW3_DXIL(name) Blob{nullptr, 0}
#endif
#ifdef DW3_HAVE_METAL
#define DW3_MSL(name) Blob{shaders::name##_msl, shaders::name##_msl_size}
#else
#define DW3_MSL(name) Blob{nullptr, 0}
#endif
#define DW3_SHADER(name) make(Blob{shaders::name##_spv, shaders::name##_spv_size}, DW3_DXIL(name), DW3_MSL(name))
    draw_vs = DW3_SHADER(psx_draw_vert);
    draw_fs = layer ? DW3_SHADER(psx_draw_layer_frag) : DW3_SHADER(psx_draw_frag);
    present_vs = DW3_SHADER(present_vert);
    present_fs = DW3_SHADER(present_frag);
    copy_vs = DW3_SHADER(vram_copy_vert);
    copy_fs = DW3_SHADER(vram_copy_frag);
    osd_vs = DW3_SHADER(osd_vert);
    osd_fs = DW3_SHADER(osd_frag);
#undef DW3_SHADER
#undef DW3_DXIL
#undef DW3_MSL
    if (!draw_vs || !draw_fs || !present_vs || !present_fs || !copy_vs || !copy_fs || !osd_vs || !osd_fs) return false;

    const RenderDescriptorRange range(RenderDescriptorRangeType::TEXTURE, 0, 1);
    const RenderDescriptorSetDesc set_desc(&range, 1);
    // The draw and display passes read two textures (VRAM and the display layer); without the layer the second
    // binding repeats the first and is never read.
    const RenderDescriptorRange pair_ranges[2] = {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                                                  RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1)};
    const RenderDescriptorSetDesc pair_desc(pair_ranges, 2);
    // The draw pass of the display layer variant also reads the replacement atlas.
    const RenderDescriptorRange triple_ranges[3] = {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                                                    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
                                                    RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 2, 1)};
    const RenderDescriptorSetDesc triple_desc(triple_ranges, 3);
    vram_set = device->createDescriptorSet(set_desc);
    present_set = device->createDescriptorSet(set_desc);
    draw_set = device->createDescriptorSet(layer ? triple_desc : pair_desc);
    display_set = device->createDescriptorSet(pair_desc);
    vram_set->setTexture(0, tex.get(), RenderTextureLayout::SHADER_READ);
    present_set->setTexture(0, rt.get(), RenderTextureLayout::SHADER_READ);
    draw_set->setTexture(0, tex.get(), RenderTextureLayout::SHADER_READ);
    draw_set->setTexture(1, layer ? hi_tex.get() : tex.get(), RenderTextureLayout::SHADER_READ);
    if (layer) draw_set->setTexture(2, atlas.get(), RenderTextureLayout::SHADER_READ);
    display_set->setTexture(0, rt.get(), RenderTextureLayout::SHADER_READ);
    display_set->setTexture(1, layer ? hi.get() : rt.get(), RenderTextureLayout::SHADER_READ);
    if (nat) {
        nat_set = device->createDescriptorSet(set_desc);
        nat_set->setTexture(0, nat.get(), RenderTextureLayout::SHADER_READ);
    }

    {
        const RenderPushConstantRange pc(0, 0, 0, 16, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = layer ? &triple_desc : &pair_desc;
        ld.descriptorSetDescsCount = 1;
        ld.allowInputLayout = true;
        draw_layout = device->createPipelineLayout(ld);

        input_slot = RenderInputSlot(0, sizeof(HwVertex));
        input_elements = {
            RenderInputElement("TEXCOORD", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(HwVertex, x)),
            RenderInputElement("TEXCOORD", 1, 1, RenderFormat::R32G32B32A32_SINT, 0, offsetof(HwVertex, plane)),
            RenderInputElement("TEXCOORD", 2, 2, RenderFormat::R32G32B32A32_SINT, 0, offsetof(HwVertex, plane) + 16),
            RenderInputElement("TEXCOORD", 3, 3, RenderFormat::R32G32B32A32_SINT, 0, offsetof(HwVertex, plane) + 32),
            RenderInputElement("TEXCOORD", 4, 4, RenderFormat::R32G32B32_SINT, 0, offsetof(HwVertex, plane) + 48),
            RenderInputElement("TEXCOORD", 5, 5, RenderFormat::R32G32B32_UINT, 0, offsetof(HwVertex, state0)),
            RenderInputElement("TEXCOORD", 6, 6, RenderFormat::R32_UINT, 0, offsetof(HwVertex, uv_rect)),
            RenderInputElement("TEXCOORD", 7, 7, RenderFormat::R32_UINT, 0, offsetof(HwVertex, hd_offset)),
        };
        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = draw_layout.get();
        pd.vertexShader = draw_vs.get();
        pd.pixelShader = draw_fs.get();
        pd.inputSlots = &input_slot;
        pd.inputSlotsCount = 1;
        pd.inputElements = input_elements.data();
        pd.inputElementsCount = static_cast<uint32_t>(input_elements.size());
        pd.renderTargetFormat[0] = kVramFormat;
        pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pd.renderTargetCount = 1;
        if (layer) {
            pd.renderTargetFormat[1] = kLayerFormat;
            pd.renderTargetBlend[1] = RenderBlendDesc::Copy();
            pd.renderTargetCount = 2;
        }
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        draw_pipeline = device->createGraphicsPipeline(pd);
    }
    {
        const RenderPushConstantRange pc(0, 0, 0, sizeof(PresentParams),
                                         RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &pair_desc;
        ld.descriptorSetDescsCount = 1;
        present_layout = device->createPipelineLayout(ld);

        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = present_layout.get();
        pd.vertexShader = present_vs.get();
        pd.pixelShader = present_fs.get();
        pd.renderTargetFormat[0] = swap_format;
        pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pd.renderTargetCount = 1;
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        present_pipeline = device->createGraphicsPipeline(pd);
    }
    {
        const RenderPushConstantRange pc(0, 0, 0, sizeof(CopyParams), RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &set_desc;
        ld.descriptorSetDescsCount = 1;
        copy_layout = device->createPipelineLayout(ld);

        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = copy_layout.get();
        pd.vertexShader = copy_vs.get();
        pd.pixelShader = copy_fs.get();
        pd.renderTargetFormat[0] = kVramFormat;
        pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pd.renderTargetCount = 1;
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        copy_pipeline = device->createGraphicsPipeline(pd);
    }
    {
        const RenderPushConstantRange pc(0, 0, 0, sizeof(OsdParams),
                                         RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &set_desc;
        ld.descriptorSetDescsCount = 1;
        osd_layout = device->createPipelineLayout(ld);

        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = osd_layout.get();
        pd.vertexShader = osd_vs.get();
        pd.pixelShader = osd_fs.get();
        pd.renderTargetFormat[0] = swap_format;
        pd.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        pd.renderTargetCount = 1;
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        osd_pipeline = device->createGraphicsPipeline(pd);
    }
    return draw_layout && draw_pipeline && present_layout && present_pipeline && copy_layout && copy_pipeline &&
           osd_layout && osd_pipeline;
}

bool HwRenderer::Impl::create_swapchain(const NativeWindow& window) {
#if defined(_WIN32)
    RenderWindow w = static_cast<RenderWindow>(window.hwnd);
#elif defined(__APPLE__)
    RenderWindow w{};
    w.window = window.cocoa_window;
    w.view = window.metal_layer;
#else
    RenderWindow w{};
    w.userData = window.user;
    w.createSurface = window.create_surface;
    w.getSize = window.get_size;
#endif
    swap_format = kSwapFormat;
    hdr_active = false;
    if (hdr_wanted) {
        RenderSwapChainDesc hdr_desc(w, RenderFormat::R16G16B16A16_FLOAT, 2);
        hdr_desc.hdr = true;
        swap = queue->createSwapChain(hdr_desc);
        if (swap && swap->resize() && swap->hdrActive) {
            swap_format = RenderFormat::R16G16B16A16_FLOAT;
            hdr_active = true;
        } else {
            swap.reset();  // not offered: the plain swap chain below takes over
        }
    }
    if (!swap) swap = queue->createSwapChain(RenderSwapChainDesc(w, kSwapFormat, 2));
    if (!swap || !swap->resize()) return false;
    for (Slot& sl : slots) sl.acquire_sem = device->createCommandSemaphore();
    create_swap_framebuffers();
    return !swap_fbs.empty();
}

// Builds the swap chain again, after hdr_wanted changed: the pipelines that draw into it depend on its format.
bool HwRenderer::Impl::recreate_swapchain() {
    if (!has_window) return true;
    wait_all();
    swap_fbs.clear();
    release_sems.clear();
    post_chain.reset();
    swap.reset();
    if (!create_swapchain(native_window)) return false;
    return create_pipelines();
}

void HwRenderer::Impl::create_swap_framebuffers() {
    swap_fbs.clear();
    release_sems.clear();
    for (uint32_t i = 0; i < swap->getTextureCount(); ++i) {
        const RenderTexture* t = swap->getTexture(i);
        RenderFramebufferDesc fbd;
        fbd.colorAttachments = &t;
        fbd.colorAttachmentsCount = 1;
        swap_fbs.push_back(device->createFramebuffer(fbd));
        release_sems.push_back(device->createCommandSemaphore());
    }
}

void HwRenderer::Impl::clear_vram() {
    ensure_recording();
    transition(rt.get(), rt_layout, RenderTextureLayout::COLOR_WRITE);
    if (layer) transition(hi.get(), hi_layout, RenderTextureLayout::COLOR_WRITE);
    cmd->setFramebuffer(layer ? draw_fb.get() : rt_fb.get());
    cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    if (layer) cmd->clearColor(1, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    transition(rt.get(), rt_layout, RenderTextureLayout::COPY_SOURCE);
    transition(tex.get(), tex_layout, RenderTextureLayout::COPY_DEST);
    cmd->copyTexture(tex.get(), rt.get());
    if (layer) {
        transition(hi.get(), hi_layout, RenderTextureLayout::COPY_SOURCE);
        transition(hi_tex.get(), hi_tex_layout, RenderTextureLayout::COPY_DEST);
        cmd->copyTexture(hi_tex.get(), hi.get());
    }
    submit();
}

// -------------------------------------------------------------------------------------------------------
// Submission and buffers

void HwRenderer::Impl::submit(RenderCommandSemaphore* wait, RenderCommandSemaphore* signal) {
    ensure_recording();
    cmd->end();
    recording = false;
    const RenderCommandList* list = cmd;
    RenderCommandSemaphore* waits[1] = {wait};
    RenderCommandSemaphore* signals[1] = {signal};
    Slot& s = slots[cur_slot];
    queue->executeCommandLists(&list, 1, wait ? waits : nullptr, wait ? 1 : 0, signal ? signals : nullptr,
                               signal ? 1 : 0, s.fence.get());
    s.pending = true;
    last_submitted = cur_slot;
    // The next slot's buffers are free once its own earlier work finishes; that is waited for on first use.
    cur_slot = (cur_slot + 1) % kFramesInFlight;
    bind_slot();
    vertex_used = 0;
    staging_used = 0;
    ++stats.submits;
}

std::size_t HwRenderer::Impl::staging_alloc(std::size_t bytes) {
    assert(bytes <= kStagingBytes);
    ensure_recording();
    if (staging_used + bytes > kStagingBytes) {
        record_draws();
        submit();
        ensure_recording();
    }
    const std::size_t off = staging_used;
    staging_used = align_up(staging_used + bytes, 512);
    return off;
}

void HwRenderer::Impl::record_draws() {
    if (pending.empty()) return;
    const std::size_t bytes = pending.size() * sizeof(HwVertex);
    if (vertex_used + bytes > kVertexBufferBytes) submit();  // earlier queued work first, then start over
    ensure_recording();
    std::memcpy(vertex_map + vertex_used, pending.data(), bytes);
    const uint32_t base = static_cast<uint32_t>(vertex_used / sizeof(HwVertex));
    vertex_used += bytes;

    transition(rt.get(), rt_layout, RenderTextureLayout::COLOR_WRITE);
    transition(tex.get(), tex_layout, RenderTextureLayout::SHADER_READ);
    if (layer) {
        transition(hi.get(), hi_layout, RenderTextureLayout::COLOR_WRITE);
        transition(hi_tex.get(), hi_tex_layout, RenderTextureLayout::SHADER_READ);
        transition(atlas.get(), atlas_layout, RenderTextureLayout::SHADER_READ);
    }
    cmd->setFramebuffer(layer ? draw_fb.get() : rt_fb.get());
    // The pipeline has one viewport and scissor per render target.
    const uint32_t targets = layer ? 2 : 1;
    const RenderViewport viewports[2] = {RenderViewport(0.0f, 0.0f, static_cast<float>(target_width()) * scale, 512.0f * scale),
                                         RenderViewport(0.0f, 0.0f, static_cast<float>(target_width()) * scale, 512.0f * scale)};
    cmd->setViewports(viewports, targets);
    cmd->setGraphicsPipelineLayout(draw_layout.get());
    cmd->setPipeline(draw_pipeline.get());
    cmd->setGraphicsDescriptorSet(draw_set.get(), 0);
    const uint32_t constants[4] = {static_cast<uint32_t>(scale), draw_mode, static_cast<uint32_t>(target_width()), 0};
    cmd->setGraphicsPushConstants(0, constants);
    const RenderVertexBufferView view(vertex_buf->at(0), static_cast<uint32_t>(kVertexBufferBytes));
    cmd->setVertexBuffers(0, &view, 1, &input_slot);
    for (const DrawRange& r : ranges) {
        const RenderRect rect(r.scissor.x0 * scale, r.scissor.y0 * scale, r.scissor.x1 * scale, r.scissor.y1 * scale);
        const RenderRect scissors[2] = {rect, rect};
        cmd->setScissors(scissors, targets);
        cmd->drawInstanced(r.count, 1, base + r.first, 0);
        ++stats.draw_calls;
    }
    stats.vertices += pending.size();
    pending.clear();
    ranges.clear();
}

// Brings the sampled copy up to date for every region changed since the last time.
void HwRenderer::Impl::sync_copy() {
    if (stale.empty()) return;
    record_draws();
    ensure_recording();
    transition(rt.get(), rt_layout, RenderTextureLayout::COPY_SOURCE);
    transition(tex.get(), tex_layout, RenderTextureLayout::COPY_DEST, true);
    if (layer) {
        transition(hi.get(), hi_layout, RenderTextureLayout::COPY_SOURCE);
        transition(hi_tex.get(), hi_tex_layout, RenderTextureLayout::COPY_DEST, true);
    }
    for (const Region& r : stale.take_all()) {
        const RenderBox box(r.x0 * scale, r.y0 * scale, r.x1 * scale, r.y1 * scale);
        cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex.get()),
                               RenderTextureCopyLocation::Subresource(rt.get()), r.x0 * scale, r.y0 * scale, 0, &box);
        if (layer)
            cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(hi_tex.get()),
                                   RenderTextureCopyLocation::Subresource(hi.get()), r.x0 * scale, r.y0 * scale, 0, &box);
    }
    ++stats.copy_syncs;
}

// Sets up the render target and copy pipeline for a fill/copy pass.
void HwRenderer::Impl::begin_vram_pass(const CopyParams& p) {
    record_draws();
    ensure_recording();
    transition(rt.get(), rt_layout, RenderTextureLayout::COLOR_WRITE);
    transition(tex.get(), tex_layout, RenderTextureLayout::SHADER_READ);
    cmd->setFramebuffer(rt_fb.get());
    cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(target_width()) * scale, 512.0f * scale));
    cmd->setGraphicsPipelineLayout(copy_layout.get());
    cmd->setPipeline(copy_pipeline.get());
    cmd->setGraphicsDescriptorSet(vram_set.get(), 0);
    cmd->setGraphicsPushConstants(0, &p);
}

// One copy over a rectangle that is read from the sampled copy, so the sampled copy must be current for the
// source (and for the destination when the mask is checked).
void HwRenderer::Impl::copy_pass(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) {
    const VramRect dst{dx, dy, src.w, src.h};
    const Region sb = region_from(src), db = region_from(dst);
    if (stale.intersects(sb) || (check_mask && stale.intersects(db))) sync_copy();
    CopyParams p{};
    p.rect[0] = src.x & (kVramWidth - 1);
    p.rect[1] = src.y & (kVramHeight - 1);
    p.rect[2] = dx & (kVramWidth - 1);
    p.rect[3] = dy & (kVramHeight - 1);
    p.flags = (check_mask ? 2u : 0u) | (set_mask ? 4u : 0u);
    p.scale = scale;
    begin_vram_pass(p);
    Region pieces[4];
    const int n = split_wrapped(dst, pieces);
    for (int i = 0; i < n; ++i) {
        const Region& r = pieces[i];
        cmd->setScissors(RenderRect(r.x0 * scale, r.y0 * scale, r.x1 * scale, r.y1 * scale));
        cmd->drawInstanced(3, 1, 0, 0);
        ++stats.draw_calls;
    }
    stale.add(db);
    dirty.add(db);
}

// -------------------------------------------------------------------------------------------------------
// Primitives

void HwRenderer::Impl::queue_primitive(const DrawState& s, const Region& bounds, bool reads_dest, const Region* tex_src,
                                       const Region* clut_src, std::size_t vertices_before) {
    // Everything this primitive reads must be in the sampled copy. If an earlier write (queued or recorded)
    // touched those areas, the queued draws are recorded first and the copy refreshed.
    const bool need_sync = (reads_dest && stale.intersects(bounds)) || (tex_src && stale.intersects(*tex_src)) ||
                           (clut_src && !clut_src->empty() && stale.intersects(*clut_src));
    if (need_sync) {
        // The new vertices are already at the tail of `pending`; set them aside while the older ones are recorded.
        std::vector<HwVertex> mine(pending.begin() + static_cast<std::ptrdiff_t>(vertices_before), pending.end());
        pending.resize(vertices_before);
        sync_copy();
        pending = std::move(mine);
        vertices_before = 0;
    }
    const Region clip = clip_region(s);
    const uint32_t first = static_cast<uint32_t>(vertices_before);
    const uint32_t count = static_cast<uint32_t>(pending.size() - vertices_before);
    if (!ranges.empty() && ranges.back().scissor == clip && ranges.back().first + ranges.back().count == first) {
        ranges.back().count += count;
    } else {
        ranges.push_back({first, count, clip});
    }
    stale.add(bounds);
    if (bounds.x0 < kVramWidth) dirty.add(bounds);  // the wide frame copy is never read back
    if (pending.size() >= kMaxPendingVertices) record_draws();
}

// Draws the copy shader over the native-pixel region `r` of `fb` (the scaled target for flag 8, the native
// texture for flag 16), reading from `source`. The source texture must already be in the shader-read layout.
void HwRenderer::Impl::scaled_pass(RenderFramebuffer* fb, RenderDescriptorSet* source, uint32_t flags, const Region& r) {
    const bool to_rt = fb == rt_fb.get();
    if (to_rt) transition(rt.get(), rt_layout, RenderTextureLayout::COLOR_WRITE);
    else transition(nat.get(), nat_layout, RenderTextureLayout::COLOR_WRITE);
    CopyParams p{};
    p.flags = flags;
    p.scale = scale;
    const int mul = to_rt ? scale : 1;
    cmd->setFramebuffer(fb);
    cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(to_rt ? target_width() : kVramWidth) * mul, 512.0f * mul));
    cmd->setScissors(RenderRect(r.x0 * mul, r.y0 * mul, r.x1 * mul, r.y1 * mul));
    cmd->setGraphicsPipelineLayout(copy_layout.get());
    cmd->setPipeline(copy_pipeline.get());
    cmd->setGraphicsDescriptorSet(source, 0);
    cmd->setGraphicsPushConstants(0, &p);
    cmd->drawInstanced(3, 1, 0, 0);
    ++stats.draw_calls;
}

void HwRenderer::Impl::upload(std::span<const uint16_t> vram, const Region& r) {
    record_draws();
    const uint32_t w = static_cast<uint32_t>(r.width()), h = static_cast<uint32_t>(r.height());
    const uint32_t pitch = upload_row_pixels(w);
    const std::size_t off = staging_alloc(static_cast<std::size_t>(pitch) * h * 4);
    auto* dst = reinterpret_cast<uint32_t*>(staging_map + off);
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t* src = vram.data() + static_cast<std::size_t>(r.y0 + static_cast<int>(y)) * kVramWidth + r.x0;
        uint32_t* row = dst + static_cast<std::size_t>(y) * pitch;
        for (uint32_t x = 0; x < w; ++x) row[x] = word_to_rgba(src[x]);
    }
    ensure_recording();
    if (scale > 1) {
        // Stage at native resolution, then replicate into the scaled target on the GPU.
        transition(nat.get(), nat_layout, RenderTextureLayout::COPY_DEST, true);
        cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(nat.get()),
                               RenderTextureCopyLocation::PlacedFootprint(staging_buf, kVramFormat, w, h, 1, pitch, off),
                               r.x0, r.y0, 0);
        transition(nat.get(), nat_layout, RenderTextureLayout::SHADER_READ);
        scaled_pass(rt_fb.get(), nat_set.get(), 8, r);
    } else {
        transition(rt.get(), rt_layout, RenderTextureLayout::COPY_DEST, true);
        cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(rt.get()),
                               RenderTextureCopyLocation::PlacedFootprint(staging_buf, kVramFormat, w, h, 1, pitch, off),
                               r.x0, r.y0, 0);
    }
    stale.add(r);
    ++stats.uploads;
}

void HwRenderer::Impl::readback(const std::vector<Region>& in, std::span<uint16_t> vram) {
    std::vector<Region> regions = in;
    auto total_bytes = [&] {
        std::size_t t = 0;
        for (const Region& r : regions)
            t += align_up(static_cast<std::size_t>(upload_row_pixels(static_cast<uint32_t>(r.width()))) * r.height() * 4, 512);
        return t;
    };
    if (total_bytes() > kReadbackBytes) {
        Region all;
        for (const Region& r : regions) all = all.united(r);
        regions.assign(1, all);
    }
    record_draws();
    ensure_recording();
    RenderTexture* source = rt.get();
    RenderTextureLayout* source_layout = &rt_layout;
    if (scale > 1) {
        // Keep the top-left subpixel of every native pixel, which is what the CPU array holds for a pixel.
        transition(rt.get(), rt_layout, RenderTextureLayout::SHADER_READ);
        for (const Region& r : regions) scaled_pass(nat_fb.get(), present_set.get(), 16, r);
        source = nat.get();
        source_layout = &nat_layout;
    }
    transition(source, *source_layout, RenderTextureLayout::COPY_SOURCE);
    std::size_t off = 0;
    std::vector<std::size_t> offsets;
    for (const Region& r : regions) {
        const uint32_t w = static_cast<uint32_t>(r.width()), h = static_cast<uint32_t>(r.height());
        const uint32_t pitch = upload_row_pixels(w);
        const RenderBox box(r.x0, r.y0, r.x1, r.y1);
        cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback_buf.get(), kVramFormat, w, h, 1, pitch, off),
                               RenderTextureCopyLocation::Subresource(source), 0, 0, 0, &box);
        offsets.push_back(off);
        off += align_up(static_cast<std::size_t>(pitch) * h * 4, 512);
    }
    submit();
    wait_slot(slots[last_submitted]);  // the copy depends on everything queued before it, and on nothing after
    const auto* data = static_cast<const uint8_t*>(readback_buf->map());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const Region& r = regions[i];
        const uint32_t pitch = upload_row_pixels(static_cast<uint32_t>(r.width()));
        for (int y = 0; y < r.height(); ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(data + offsets[i]) + static_cast<std::size_t>(y) * pitch;
            uint16_t* dst = vram.data() + static_cast<std::size_t>(r.y0 + y) * kVramWidth + r.x0;
            for (int x = 0; x < r.width(); ++x) dst[x] = rgba_to_word(row[x]);
        }
    }
    readback_buf->unmap();
    ++stats.readbacks;
}

// -------------------------------------------------------------------------------------------------------
// Texture replacement

void HwRenderer::Impl::upload_replacement(const texrep::Image& img, int x, int y) {
    const uint32_t w = static_cast<uint32_t>(img.w), h = static_cast<uint32_t>(img.h);
    const uint32_t pitch = upload_row_pixels(w);
    const std::size_t off = staging_alloc(static_cast<std::size_t>(pitch) * h * 4);
    auto* dst = reinterpret_cast<uint32_t*>(staging_map + off);
    for (uint32_t row = 0; row < h; ++row)
        std::memcpy(dst + static_cast<std::size_t>(row) * pitch, img.rgba.data() + static_cast<std::size_t>(row) * w, w * 4);
    ensure_recording();
    transition(atlas.get(), atlas_layout, RenderTextureLayout::COPY_DEST, true);
    cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(atlas.get()),
                           RenderTextureCopyLocation::PlacedFootprint(staging_buf, kVramFormat, w, h, 1, pitch, off),
                           static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0);
}

// The place of a pack entry's image in the atlas, loading it on first use. When the atlas is full everything is
// dropped and loaded again as needed (after the draws that still read the old contents have been recorded).
const HwRenderer::Impl::Resident* HwRenderer::Impl::make_resident(const texrep::PackEntry& e) {
    const std::pair<uint64_t, uint64_t> key{e.block, e.clut};
    if (auto it = resident.find(key); it != resident.end()) return &it->second;
    if (unusable.count(key)) return nullptr;
    texrep::Image img;
    std::string err;
    if (!pack->load_image(e, img, err)) {
        std::fprintf(stderr, "texture pack: %s\n", err.c_str());
        unusable.insert(key);
        return nullptr;
    }
    auto place = atlas_alloc.alloc(img.w, img.h);
    if (!place) {
        record_draws();
        resident.clear();
        atlas_alloc.reset();
        ++atlas_epoch;
        place = atlas_alloc.alloc(img.w, img.h);
        if (!place) {
            unusable.insert(key);
            return nullptr;
        }
    }
    upload_replacement(img, place->first, place->second);
    return &(resident[key] = Resident{place->first, place->second});
}

// Handles a sprite that samples 8-bit texels: reports what it draws to the dump, and draws the parts that show a
// block with a replacement from the atlas. Returns false when nothing is replaced, so the caller draws the sprite as
// usual.
bool HwRenderer::Impl::draw_background_rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f,
                                            const GpuVertex& v, int width, int height) {
    const Region clut = clut_region(s, f);
    if (clut.empty() || dirty.intersects(clut)) return false;  // the CPU copy of the palette may be stale
    const int clut_x = (f.clut & 0x3F) * 16, clut_y = (f.clut >> 6) & 0x1FF;
    std::vector<texrep::Piece> pieces = texrep::split_by_blocks(blocks, s.tex_base_x, s.tex_base_y, v.u, v.v, width, height, true);
    bool any_block = false;
    for (const texrep::Piece& p : pieces) any_block |= p.block != nullptr;
    if (!any_block) return false;
    const uint64_t clut_hash = texrep::hash_clut(vram, clut_x, clut_y);

    struct Draw {
        const texrep::Piece* piece;
        Resident slot;
    };
    std::vector<Draw> replaced;
    for (int attempt = 0; attempt < 2; ++attempt) {
        replaced.clear();
        const int epoch = atlas_epoch;
        for (const texrep::Piece& p : pieces) {
            if (!p.block) continue;
            const uint64_t bh = blocks.hash_of(*p.block, vram);
            if (dump && p.w * p.h >= 16) {
                if (!dump->has_variant(bh, clut_hash)) {
                    std::vector<uint32_t> rgba;
                    texrep::decode_block_8bit(vram, p.block->rect, clut_x, clut_y, rgba);
                    dump->add_variant(bh, clut_hash, p.block->width_texels(), p.block->height_texels(), std::move(rgba));
                }
                dump->observe(bh, clut_hash, (v.x - s.off_x) + (p.u - v.u) - (p.u - p.block_u0),
                              (v.y - s.off_y) + (p.v - v.v) - (p.v - p.block_v0));
            }
            if (!pack) continue;
            const texrep::PackEntry* e = pack->find(bh, clut_hash);
            if (!e || e->width != p.block->width_texels() || e->height != p.block->height_texels()) continue;
            if (const Resident* r = make_resident(*e)) replaced.push_back({&p, *r});
        }
        if (epoch == atlas_epoch) break;  // otherwise earlier pieces were evicted by a later one: resolve again
    }
    if (replaced.empty()) return false;

    const int scale_hd = pack->scale();
    auto is_replaced = [&](const texrep::Piece& p) {
        for (const Draw& d : replaced)
            if (d.piece == &p) return &d;
        return static_cast<const Draw*>(nullptr);
    };
    for (const texrep::Piece& p : pieces) {
        GpuVertex pv = v;
        pv.x = v.x + (p.u - v.u);
        pv.y = v.y + (p.v - v.v);
        pv.u = static_cast<uint8_t>(p.u);
        pv.v = static_cast<uint8_t>(p.v);
        const Region bounds = rect_bounds(s, pv, p.w, p.h);
        if (bounds.empty()) continue;
        const std::size_t before = pending.size();
        build_rect(pending, s, f, pv, p.w, p.h);
        if (const Draw* d = is_replaced(p)) {
            const texrep::ReplacementOffset o =
                texrep::replacement_offset(p.block_u0, p.block_v0, d->slot.x, d->slot.y, scale_hd);
            for (std::size_t i = before; i < pending.size(); ++i) {
                pending[i].state0 |= state_bits::kHd | (static_cast<uint32_t>(scale_hd - 1) << state_bits::kHdScaleShift);
                pending[i].hd_offset = (static_cast<uint32_t>(o.y) << 16) | (static_cast<uint32_t>(o.x) & 0xFFFFu);
            }
        }
        const Region t = rect_texture_region(s, pv, p.w, p.h);
        queue_primitive(s, bounds, f.semi_transparent || s.check_mask, &t, &clut, before);
    }
    return true;
}

// -------------------------------------------------------------------------------------------------------
// Presentation

const PostFx* HwRenderer::Impl::effective_post(const PostFx* in, PostFx& storage) {
    if (hdr_active) {
        storage = in ? *in : PostFx{};
        if (storage.hdr.mode == HdrMode::Off) storage.hdr = hdr_params;
        storage.hdr.mode = HdrMode::ScRgb;
        hdr_params = storage.hdr;
        return &storage;
    }
    if (has_window && in && in->hdr.mode != HdrMode::Off) {
        storage = *in;
        storage.hdr.mode = HdrMode::Off;
        return &storage;
    }
    return in;
}

// The picture to show: the wide frame copy while it is being drawn and the display is one of the frame buffers.
DisplayState HwRenderer::Impl::wide_display(const DisplayState& d) const {
    if (!mirror || !wide.enabled() || !d.enabled || d.color24 || d.width != wide.frame_w || d.height != wide.frame_h ||
        d.vram_x != 0 || (d.vram_y != 0 && d.vram_y != 256) || !wide_ready[d.vram_y == 256 ? 1 : 0])
        return d;
    DisplayState w = d;
    w.vram_x = WideFrame::kOriginX;
    w.width = wide.width();
    w.aspect_num = wide.width();
    w.aspect_den = wide.frame_h;
    return w;
}

void HwRenderer::Impl::draw_display(RenderFramebuffer* fb, uint32_t w, uint32_t h, const DisplayState& d, ScaleMode mode, bool linear,
                                    const PostChain::Plan* plan) {
    cmd->setFramebuffer(fb);
    cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
    cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)));
    cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    if (!d.enabled) return;
    const PresentRect pr = compute_present_rect(d.width, d.height, static_cast<int>(w), static_cast<int>(h), mode, d.aspect_num, d.aspect_den);
    if (pr.w <= 0 || pr.h <= 0) return;
    if (plan) {
        post_chain->draw(cmd, *plan, nullptr, w, h, pr.x, pr.y, pr.w, pr.h);
        ++stats.draw_calls;
        return;
    }
    PresentParams p{};
    p.dest[0] = static_cast<float>(pr.x);
    p.dest[1] = static_cast<float>(pr.y);
    p.dest[2] = static_cast<float>(pr.w);
    p.dest[3] = static_cast<float>(pr.h);
    p.window[0] = static_cast<float>(w);
    p.window[1] = static_cast<float>(h);
    p.src[0] = d.vram_x;
    p.src[1] = d.vram_y;
    p.src[2] = d.width;
    p.src[3] = d.height;
    p.color24 = d.color24 ? 1 : 0;
    p.linear = linear ? 1 : 0;
    p.scale = scale;
    p.layer = layer ? 1 : 0;
    push_present(p);
}

void HwRenderer::Impl::push_present(const PresentParams& p) {
    cmd->setGraphicsPipelineLayout(present_layout.get());
    cmd->setPipeline(present_pipeline.get());
    cmd->setGraphicsDescriptorSet(display_set.get(), 0);
    cmd->setGraphicsPushConstants(0, &p);
    cmd->drawInstanced(6, 1, 0, 0);
    ++stats.draw_calls;
}

// Post-processing: draws the display area 1:1 (nearest) into the chain's source image, then runs the passes up to the
// last one, which draw_display repeats for each window-sized target.
bool HwRenderer::Impl::run_post(const PostFx& fx, const DisplayState& d, const PresentRect& pr, PostChain::Plan& plan) {
    if (!post_chain) post_chain = std::make_unique<PostChain>(device.get(), iface->getCapabilities().shaderFormat, swap_format);
    const uint32_t k = d.color24 ? 1u : static_cast<uint32_t>(scale);
    const uint32_t sw = static_cast<uint32_t>(d.width) * k, sh = static_cast<uint32_t>(d.height) * k;
    if (!post_chain->prepare(cmd, fx, sw, sh, static_cast<uint32_t>(pr.w), static_cast<uint32_t>(pr.h), [this] { wait_all(); }))
        return false;
    PostChain::Target& source = post_chain->source(cmd);
    cmd->setFramebuffer(source.fb.get());
    cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh)));
    cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(sw), static_cast<int32_t>(sh)));
    PresentParams p{};
    p.dest[2] = static_cast<float>(sw);
    p.dest[3] = static_cast<float>(sh);
    p.window[0] = static_cast<float>(sw);
    p.window[1] = static_cast<float>(sh);
    p.src[0] = d.vram_x;
    p.src[1] = d.vram_y;
    p.src[2] = d.width;
    p.src[3] = d.height;
    p.color24 = d.color24 ? 1 : 0;
    p.scale = scale;
    p.layer = layer ? 1 : 0;
    push_present(p);
    return post_chain->run(cmd, fx, static_cast<uint32_t>(pr.w), static_cast<uint32_t>(pr.h), static_cast<float>(d.height), plan);
}

// Copies the overlay image into `osd_tex` (recreated when its size changes). Must run before the frame's render
// pass is recorded.
bool HwRenderer::Impl::upload_overlay(const HwOverlay& o) {
    const uint32_t w = static_cast<uint32_t>(o.w), h = static_cast<uint32_t>(o.h);
    if (!osd_tex || osd_w != w || osd_h != h) {
        wait_all();  // earlier frames may still sample the old texture
        osd_set.reset();
        const RenderDescriptorRange range(RenderDescriptorRangeType::TEXTURE, 0, 1);
        osd_tex = device->createTexture(RenderTextureDesc::Texture2D(w, h, 1, kVramFormat));
        if (!osd_tex) return false;
        osd_set = device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1));
        osd_set->setTexture(0, osd_tex.get(), RenderTextureLayout::SHADER_READ);
        osd_tex_layout = RenderTextureLayout::UNKNOWN;
        osd_w = w;
        osd_h = h;
        osd_serial = 0;
    } else if (osd_serial == o.serial) {
        return true;
    }
    const uint32_t pitch = upload_row_pixels(w);
    const std::size_t off = staging_alloc(static_cast<std::size_t>(pitch) * h * 4);
    auto* dst = reinterpret_cast<uint32_t*>(staging_map + off);
    for (uint32_t y = 0; y < h; ++y) std::memcpy(dst + static_cast<std::size_t>(y) * pitch, o.rgba + static_cast<std::size_t>(y) * w, w * 4);
    transition(osd_tex.get(), osd_tex_layout, RenderTextureLayout::COPY_DEST, true);
    cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(osd_tex.get()),
                           RenderTextureCopyLocation::PlacedFootprint(staging_buf, kVramFormat, w, h, 1, pitch, off), 0, 0, 0);
    transition(osd_tex.get(), osd_tex_layout, RenderTextureLayout::SHADER_READ);
    osd_serial = o.serial;
    return true;
}

// Alpha-blends the uploaded overlay into the framebuffer that is already bound by draw_display.
void HwRenderer::Impl::draw_overlay(uint32_t w, uint32_t h, const HwOverlay& o) {
    OsdParams p{};
    p.dest[0] = static_cast<float>(o.x);
    p.dest[1] = static_cast<float>(o.y);
    p.dest[2] = static_cast<float>(o.w);
    p.dest[3] = static_cast<float>(o.h);
    p.window[0] = static_cast<float>(w);
    p.window[1] = static_cast<float>(h);
    p.opacity = o.opacity;
    p.hdr_scale = hdr_active ? hdr_params.paper_white_nits / kScRgbWhiteNits : 0.0f;
    cmd->setGraphicsPipelineLayout(osd_layout.get());
    cmd->setPipeline(osd_pipeline.get());
    cmd->setGraphicsDescriptorSet(osd_set.get(), 0);
    cmd->setGraphicsPushConstants(0, &p);
    cmd->drawInstanced(6, 1, 0, 0);
    ++stats.draw_calls;
}

// Converts the capture buffer (BGRA) of the last frame into RGBA8.
void HwRenderer::Impl::read_capture(uint32_t w, uint32_t h, std::vector<uint32_t>& out) {
    const uint32_t pitch = upload_row_pixels(w);
    const auto* data = static_cast<const uint32_t*>(capture_buf->map());
    out.resize(static_cast<std::size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t bgra = data[static_cast<std::size_t>(y) * pitch + x];
            out[static_cast<std::size_t>(y) * w + x] = (bgra & 0xFF00FF00u) | ((bgra & 0xFF) << 16) | ((bgra >> 16) & 0xFF) | 0xFF000000u;
        }
    }
    capture_buf->unmap();
}

bool HwRenderer::Impl::ensure_capture(uint32_t w, uint32_t h) {
    if (capture_tex && capture_w == w && capture_h == h) return true;
    capture_fb.reset();
    capture_tex = device->createTexture(RenderTextureDesc::ColorTarget(w, h, kSwapFormat));
    capture_buf = device->createBuffer(RenderBufferDesc::ReadbackBuffer(static_cast<uint64_t>(upload_row_pixels(w)) * h * 4));
    if (!capture_tex || !capture_buf) return false;
    const RenderTexture* t = capture_tex.get();
    RenderFramebufferDesc fbd;
    fbd.colorAttachments = &t;
    fbd.colorAttachmentsCount = 1;
    capture_fb = device->createFramebuffer(fbd);
    capture_layout = RenderTextureLayout::UNKNOWN;
    capture_w = w;
    capture_h = h;
    return capture_fb != nullptr;
}

// -------------------------------------------------------------------------------------------------------
// Public interface

HwRenderer::HwRenderer() : impl_(std::make_unique<Impl>()) {}

HwRenderer::~HwRenderer() {
    if (impl_ && impl_->device) {
        if (impl_->recording) impl_->submit();
        impl_->wait_all();
        for (auto& sl : impl_->slots) {
            if (sl.vertex_map) sl.vertex_buf->unmap();
            if (sl.staging_map) sl.staging_buf->unmap();
        }
    }
}

bool HwRenderer::api_compiled_in(GraphicsApi api) {
#ifdef __APPLE__
    if (api == GraphicsApi::Vulkan) return false;  // would need MoltenVK
#endif
    if (api == GraphicsApi::Metal) {
#ifdef DW3_HAVE_METAL
        return true;
#else
        return false;
#endif
    }
    if (api == GraphicsApi::D3D12) {
#ifdef DW3_HAVE_D3D12
        return true;
#else
        return false;
#endif
    }
    return true;
}

std::unique_ptr<HwRenderer> HwRenderer::create(const NativeWindow* window, const std::vector<std::string>& instance_extensions,
                                               std::string& error, int internal_scale,
                                               const HwOptions& options) {
    SetVulkanExtraInstanceExtensions(instance_extensions);
    std::unique_ptr<HwRenderer> r(new HwRenderer());
    if (!r->impl_->init(window, internal_scale, options, error)) return nullptr;
    return r;
}

DrawState HwRenderer::Impl::wide_state(const DrawState& s) {
    DrawState d = s;
    if (wide.covers_width(s.area_x1, s.area_x2)) {
        d.area_x1 = WideFrame::kOriginX;
        d.area_x2 = WideFrame::kOriginX + wide.width() - 1;
    } else {
        d.area_x1 = wide.map_x(s.area_x1);
        d.area_x2 = wide.map_x(s.area_x2);
    }
    wide_ready[s.area_y1 >= 256 ? 1 : 0] = true;
    return d;
}

void HwRenderer::Impl::draw_triangle(const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3], bool tracked) {
    const Region bounds = triangle_bounds(s, v);
    if (bounds.empty()) return;
    if (tracked && tracking()) blocks.invalidate(bounds);
    const std::size_t before = pending.size();
    if (precise && has_precise(v)) build_precise_triangle(pending, s, f, v, scale);
    else build_triangle(pending, s, f, v, scale);
    const bool tex = uses_texture(s, f);
    Region t, c;
    if (tex) {
        t = triangle_texture_region(s, v);
        c = clut_region(s, f);
    }
    queue_primitive(s, bounds, f.semi_transparent || s.check_mask, tex ? &t : nullptr, tex ? &c : nullptr, before);
}

void HwRenderer::triangle(std::span<uint16_t>, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) {
    Impl& m = *impl_;
    if (m.mirrors(s)) {
        // World geometry (drawn with a screen-centre offset) keeps its position relative to the centre; flat 2D
        // shapes that touch the frame edge stretch to the edge of the wide frame.
        const bool world = s.off_x != 0 || s.off_y != 0;
        const bool stretch = !world && !uses_texture(s, f);
        GpuVertex t[3] = {v[0], v[1], v[2]};
        for (GpuVertex& g : t) {
            const int mapped = stretch ? m.wide.snap_x(g.x) : m.wide.map_x(g.x);
            if (mapped != g.x + m.wide.shift()) g.precise = false;
            g.fx += static_cast<float>(m.wide.shift());
            g.x = mapped;
        }
        m.draw_triangle(m.wide_state(s), f, t, false);
    }
    m.draw_triangle(s, f, v, true);
}

void HwRenderer::Impl::draw_rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v,
                                 int width, int height, bool tracked) {
    const Region bounds = rect_bounds(s, v, width, height);
    if (bounds.empty()) return;
    if (tracked && tracking()) {
        blocks.invalidate(bounds);
        // Opaque sprites that read 8-bit texels one to one: the candidates for background images.
        if (s.depth == TexDepth::Bit8 && uses_texture(s, f) && !f.semi_transparent && !s.rect_flip_x && !s.rect_flip_y &&
            s.win_mask_x == 0 && s.win_mask_y == 0 && draw_background_rect(vram, s, f, v, width, height))
            return;
    }
    const std::size_t before = pending.size();
    build_rect(pending, s, f, v, width, height);
    const bool tex = uses_texture(s, f);
    Region t, c;
    if (tex) {
        t = rect_texture_region(s, v, width, height);
        c = clut_region(s, f);
    }
    queue_primitive(s, bounds, f.semi_transparent || s.check_mask, tex ? &t : nullptr, tex ? &c : nullptr, before);
}

void HwRenderer::rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                      int height) {
    Impl& m = *impl_;
    if (m.mirrors(s)) {
        GpuVertex t = v;
        int w = width;
        if (!uses_texture(s, f)) {
            const int left = m.wide.snap_x(v.x), right = m.wide.snap_x(v.x + width);
            t.x = left;
            w = right - left;
        } else {
            t.x = m.wide.map_x(v.x);
        }
        m.draw_rect(vram, m.wide_state(s), f, t, w, height, false);
    }
    m.draw_rect(vram, s, f, v, width, height, true);
}

void HwRenderer::Impl::draw_line(const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a, const GpuVertex& b,
                                 bool tracked) {
    const Region bounds = line_bounds(s, a, b);
    if (bounds.empty()) return;
    if (tracked && tracking()) blocks.invalidate(bounds);
    const std::size_t before = pending.size();
    if (build_line(pending, s, f, a, b) == 0) return;
    queue_primitive(s, bounds, f.semi_transparent || s.check_mask, nullptr, nullptr, before);
}

void HwRenderer::line(std::span<uint16_t>, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
                      const GpuVertex& b) {
    Impl& m = *impl_;
    if (m.mirrors(s)) {
        GpuVertex wa = a, wb = b;
        wa.x = m.wide.map_x(a.x);
        wb.x = m.wide.map_x(b.x);
        m.draw_line(m.wide_state(s), f, wa, wb, false);
    }
    m.draw_line(s, f, a, b, true);
}

void HwRenderer::vram_written(std::span<const uint16_t> vram, const VramRect& r) {
    const Region region = region_from(r);
    if (impl_->tracking()) impl_->blocks.add_upload(region);
    impl_->upload(vram, region);
}

void HwRenderer::vram_read_begin(std::span<uint16_t> vram, const VramRect& r) {
    Impl& m = *impl_;
    const Region want = region_from(r);
    if (!m.dirty.intersects(want)) return;
    m.readback(m.dirty.take_intersecting(want), vram);
}

bool HwRenderer::vram_fill(const VramRect& r, uint16_t color) {
    Impl& m = *impl_;
    CopyParams p{};
    p.flags = 1;
    p.color = word_to_rgba(color);
    p.scale = m.scale;
    m.begin_vram_pass(p);
    Region pieces[4];
    const int n = split_wrapped(r, pieces);
    for (int i = 0; i < n; ++i) {
        const Region& q = pieces[i];
        m.cmd->setScissors(RenderRect(q.x0 * m.scale, q.y0 * m.scale, q.x1 * m.scale, q.y1 * m.scale));
        m.cmd->drawInstanced(3, 1, 0, 0);
        ++m.stats.draw_calls;
    }
    const Region b = region_from(r);
    if (m.mirror && m.wide.in_frame(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1)) {
        // The same fill in the wide copy; one that spans the frame spans the whole copy.
        const bool full = m.wide.covers_width(r.x, r.x + r.w - 1);
        const int x0 = full ? WideFrame::kOriginX : m.wide.map_x(r.x);
        const int x1 = full ? WideFrame::kOriginX + m.wide.width() : m.wide.map_x(r.x + r.w);
        m.cmd->setScissors(RenderRect(x0 * m.scale, r.y * m.scale, x1 * m.scale, (r.y + r.h) * m.scale));
        m.cmd->drawInstanced(3, 1, 0, 0);
        ++m.stats.draw_calls;
        m.stale.add({x0, r.y, x1, r.y + r.h});
        m.wide_ready[r.y >= 256 ? 1 : 0] = true;
    }
    if (m.tracking()) m.blocks.invalidate(b);
    m.stale.add(b);
    m.dirty.add(b);
    ++m.stats.fills;
    return true;
}

bool HwRenderer::vram_copy(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) {
    Impl& m = *impl_;
    if (m.tracking()) m.blocks.invalidate(region_from({dx, dy, src.w, src.h}));
    if (copy_has_row_hazard(src, dx, dy)) {
        // Rows are copied in order and later rows read what earlier ones wrote, so do them one at a time.
        for (int k = 0; k < src.h; ++k)
            m.copy_pass({src.x, src.y + k, src.w, 1}, dx, dy + k, check_mask, set_mask);
    } else {
        m.copy_pass(src, dx, dy, check_mask, set_mask);
    }
    ++m.stats.copies;
    return true;
}

void HwRenderer::read_vram(std::span<uint16_t> out) {
    impl_->readback({kWholeVram}, out);
    impl_->dirty.clear();
}

bool HwRenderer::read_scaled(const VramRect& r, std::vector<uint16_t>& out) {
    Impl& m = *impl_;
    const uint32_t w = static_cast<uint32_t>(r.w * m.scale), h = static_cast<uint32_t>(r.h * m.scale);
    const uint32_t pitch = upload_row_pixels(w);
    if (static_cast<std::size_t>(pitch) * h * 4 > kReadbackBytes) return false;
    m.record_draws();
    m.ensure_recording();
    m.transition(m.rt.get(), m.rt_layout, RenderTextureLayout::COPY_SOURCE);
    const RenderBox box(r.x * m.scale, r.y * m.scale, (r.x + r.w) * m.scale, (r.y + r.h) * m.scale);
    m.cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(m.readback_buf.get(), kVramFormat, w, h, 1, pitch, 0),
                             RenderTextureCopyLocation::Subresource(m.rt.get()), 0, 0, 0, &box);
    m.submit();
    m.wait_slot(m.slots[m.last_submitted]);
    const auto* data = static_cast<const uint32_t*>(m.readback_buf->map());
    out.resize(static_cast<std::size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) out[static_cast<std::size_t>(y) * w + x] = rgba_to_word(data[static_cast<std::size_t>(y) * pitch + x]);
    m.readback_buf->unmap();
    return true;
}

bool HwRenderer::read_display(const VramRect& r, std::vector<uint32_t>& rgba) {
    Impl& m = *impl_;
    const uint32_t w = static_cast<uint32_t>(r.w * m.scale), h = static_cast<uint32_t>(r.h * m.scale);
    const uint32_t pitch = upload_row_pixels(w);
    const std::size_t rt_bytes = align_up(static_cast<std::size_t>(pitch) * h * 4, 512), hi_bytes = static_cast<std::size_t>(pitch) * h * 8;
    if (rt_bytes + hi_bytes > kReadbackBytes) return false;
    m.record_draws();
    m.ensure_recording();
    m.transition(m.rt.get(), m.rt_layout, RenderTextureLayout::COPY_SOURCE);
    const RenderBox box(r.x * m.scale, r.y * m.scale, (r.x + r.w) * m.scale, (r.y + r.h) * m.scale);
    m.cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(m.readback_buf.get(), kVramFormat, w, h, 1, pitch, 0),
                             RenderTextureCopyLocation::Subresource(m.rt.get()), 0, 0, 0, &box);
    if (m.layer) {
        m.transition(m.hi.get(), m.hi_layout, RenderTextureLayout::COPY_SOURCE);
        m.cmd->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(m.readback_buf.get(), kLayerFormat, w, h, 1, pitch, rt_bytes),
            RenderTextureCopyLocation::Subresource(m.hi.get()), 0, 0, 0, &box);
    }
    m.submit();
    m.wait_slot(m.slots[m.last_submitted]);
    const auto* base = static_cast<const uint8_t*>(m.readback_buf->map());
    rgba.resize(static_cast<std::size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * pitch + x;
            uint32_t px = reinterpret_cast<const uint32_t*>(base)[i];
            if (m.layer) {
                const auto* q = reinterpret_cast<const uint16_t*>(base + rt_bytes) + i * 4;
                if (q[3] == rgba_to_word(px)) {
                    auto to8 = [](uint32_t v) { return (v * 255u + 32767u) / 65535u; };
                    px = to8(q[0]) | (to8(q[1]) << 8) | (to8(q[2]) << 16);
                }
            }
            rgba[static_cast<std::size_t>(y) * w + x] = (px & 0x00FFFFFFu) | 0xFF000000u;
        }
    m.readback_buf->unmap();
    return true;
}

bool HwRenderer::present(const DisplayState& shown, ScaleMode mode, bool linear, std::vector<uint32_t>* capture,
                         int* capture_w, int* capture_h, const HwOverlay* overlay, const PostFx* post) {
    Impl& m = *impl_;
    const DisplayState display = m.wide_display(shown);
    PostFx post_storage;
    post = m.effective_post(post, post_storage);
    if (m.dump) m.dump->end_frame();
    if (!m.swap) return false;
    m.record_draws();
    if (overlay && (!overlay->rgba || overlay->w <= 0 || overlay->h <= 0 || !m.upload_overlay(*overlay))) overlay = nullptr;
    if (m.swap->needsResize()) {
        m.wait_all();
        m.swap_fbs.clear();
        if (!m.swap->resize()) return false;
        m.create_swap_framebuffers();
    }
    if (m.swap->isEmpty() || m.swap_fbs.empty()) {
        m.submit();
        return false;
    }
    uint32_t index = 0;
    m.ensure_recording();  // also waits for this slot's previous frame, which owns its acquire semaphore
    RenderCommandSemaphore* acquire = m.slots[m.cur_slot].acquire_sem.get();
    if (!m.swap->acquireTexture(acquire, &index)) {
        // Out of date: rebuild on the next frame. The acquire semaphore was not signalled.
        m.submit();
        m.wait_all();
        m.swap_fbs.clear();
        if (m.swap->resize()) m.create_swap_framebuffers();
        return false;
    }
    const uint32_t w = m.swap->getWidth(), h = m.swap->getHeight();
    m.transition(m.rt.get(), m.rt_layout, RenderTextureLayout::SHADER_READ);
    if (m.layer) m.transition(m.hi.get(), m.hi_layout, RenderTextureLayout::SHADER_READ);
    PostChain::Plan plan;
    const PostChain::Plan* plan_ptr = nullptr;
    if (post && post->active() && display.enabled) {
        const PresentRect pr = compute_present_rect(display.width, display.height, static_cast<int>(w), static_cast<int>(h), mode,
                                                 display.aspect_num, display.aspect_den);
        if (pr.w > 0 && pr.h > 0 && m.run_post(*post, display, pr, plan)) plan_ptr = &plan;
    }
    RenderTexture* target = m.swap->getTexture(index);
    m.cmd->barriers(RenderBarrierStage::ALL, RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
    m.draw_display(m.swap_fbs[index].get(), w, h, display, mode, linear, plan_ptr);
    if (overlay) m.draw_overlay(w, h, *overlay);

    const bool grab = capture != nullptr && !m.hdr_active && m.ensure_capture(w, h);  // the capture target is 8-bit
    if (grab) {
        m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COLOR_WRITE);
        m.draw_display(m.capture_fb.get(), w, h, display, mode, linear, plan_ptr);
        m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COPY_SOURCE);
        const uint32_t pitch = upload_row_pixels(w);
        m.cmd->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(m.capture_buf.get(), kSwapFormat, w, h, 1, pitch, 0),
            RenderTextureCopyLocation::Subresource(m.capture_tex.get()), 0, 0, 0);
    }
    m.cmd->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
    RenderCommandSemaphore* release = m.release_sems[index].get();
    m.submit(acquire, release);
    RenderCommandSemaphore* waits[1] = {release};
    m.swap->present(index, waits, 1);

    if (grab) {
        m.wait_slot(m.slots[m.last_submitted]);
        m.read_capture(w, h, *capture);
        if (capture_w) *capture_w = static_cast<int>(w);
        if (capture_h) *capture_h = static_cast<int>(h);
    }
    return true;
}

bool HwRenderer::render_to_image(const DisplayState& shown, ScaleMode mode, bool linear, const PostFx* post, int width,
                                 int height, std::vector<uint32_t>& rgba) {
    Impl& m = *impl_;
    const DisplayState display = m.wide_display(shown);
    PostFx post_storage;
    post = m.effective_post(post, post_storage);
    if (width <= 0 || height <= 0) return false;
    const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
    m.record_draws();
    m.ensure_recording();
    if (!m.ensure_capture(w, h)) return false;
    m.transition(m.rt.get(), m.rt_layout, RenderTextureLayout::SHADER_READ);
    if (m.layer) m.transition(m.hi.get(), m.hi_layout, RenderTextureLayout::SHADER_READ);
    PostChain::Plan plan;
    const PostChain::Plan* plan_ptr = nullptr;
    if (post && post->active() && display.enabled) {
        const PresentRect pr = compute_present_rect(display.width, display.height, width, height, mode, display.aspect_num,
                                                 display.aspect_den);
        if (pr.w > 0 && pr.h > 0 && m.run_post(*post, display, pr, plan)) plan_ptr = &plan;
    }
    m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COLOR_WRITE);
    m.draw_display(m.capture_fb.get(), w, h, display, mode, linear, plan_ptr);
    m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COPY_SOURCE);
    m.cmd->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(m.capture_buf.get(), kSwapFormat, w, h, 1, upload_row_pixels(w), 0),
        RenderTextureCopyLocation::Subresource(m.capture_tex.get()), 0, 0, 0);
    m.submit();
    m.wait_slot(m.slots[m.last_submitted]);
    m.read_capture(w, h, rgba);
    return true;
}

bool HwRenderer::reconfigure(int internal_scale, const HwOptions& options, std::span<uint16_t> vram, std::string& error) {
    Impl& m = *impl_;
    const int new_scale = std::clamp(internal_scale, 1, 8);
    const bool new_layer = options.true_color || options.filter != TextureFilter::Nearest || options.texture_pack;
    const uint32_t new_mode = (new_layer ? 1u : 0u) | (options.true_color ? 2u : 0u) | (static_cast<uint32_t>(options.filter) << 2);
    m.record_draws();
    const bool pack_present_changed = static_cast<bool>(options.texture_pack) != static_cast<bool>(m.pack);
    if (options.hdr != m.hdr_wanted) {
        m.hdr_wanted = options.hdr;
        if (!m.recreate_swapchain()) {
            error = "swap chain creation failed";
            m.hdr_wanted = !options.hdr;
            m.recreate_swapchain();
            return false;
        }
    }
    const bool wide_changed = options.wide_frame.margin != m.wide.margin;
    if (new_scale == m.scale && new_layer == m.layer && !pack_present_changed && !wide_changed) {
        // Only the draw shader's mode word and the vertex path change: nothing to rebuild. A different pack empties
        // the atlas, which the new pack's images will fill again.
        if (options.texture_pack != m.pack) {
            m.resident.clear();
            m.unusable.clear();
            m.atlas_alloc.reset();
            ++m.atlas_epoch;
            m.pack = options.texture_pack;
        }
        m.precise = options.precise_geometry;
        m.wide = options.wide_frame;
        m.draw_mode = new_mode;
        return true;
    }
    // Bring the CPU copy of VRAM up to date, then rebuild the render targets and pipelines and put the image back.
    m.readback({kWholeVram}, vram);
    m.dirty.clear();
    m.wait_all();
    const int old_scale = m.scale;
    const bool old_layer = m.layer;
    const auto old_pack = m.pack;
    const WideFrame old_wide = m.wide;
    m.wide = options.wide_frame;
    m.wide_ready[0] = m.wide_ready[1] = false;
    auto rebuild = [&](int s, bool l, std::string& err) {
        m.scale = s;
        m.layer = l;
        if (!m.create_targets(err) || !m.create_pipelines()) {
            if (err.empty()) err = "pipeline creation failed";
            return false;
        }
        m.pending.clear();
        m.ranges.clear();
        m.dirty.clear();
        m.stale.clear();
        m.clear_vram();
        m.upload(vram, kWholeVram);
        return true;
    };
    m.pack = options.texture_pack;
    if (!rebuild(new_scale, new_layer, error)) {
        std::string ignored;
        m.wait_all();
        m.pack = old_pack;
        m.wide = old_wide;
        rebuild(old_scale, old_layer, ignored);
        return false;
    }
    m.precise = options.precise_geometry;
    m.draw_mode = new_mode;
    return true;
}

bool HwRenderer::hdr_active() const { return impl_->hdr_active; }

void HwRenderer::set_wide_mirror(bool active) {
    Impl& m = *impl_;
    if (active == m.mirror) return;
    m.mirror = active && m.wide.enabled();
    m.wide_ready[0] = m.wide_ready[1] = false;
}

void HwRenderer::set_texture_dump(const std::string& directory) {
    Impl& m = *impl_;
    m.dump_dir = directory;
    m.dump = directory.empty() ? nullptr : std::make_unique<texrep::TextureDump>();
}

bool HwRenderer::write_texture_dump(std::string& error) {
    Impl& m = *impl_;
    if (!m.dump) return true;
    return m.dump->write(m.dump_dir, error);
}

void HwRenderer::set_vsync(bool enabled) {
    if (impl_->swap) impl_->swap->setVsyncEnabled(enabled);
}

int HwRenderer::internal_scale() const { return impl_->scale; }

const HwStats& HwRenderer::stats() const { return impl_->stats; }

std::string HwRenderer::device_name() const { return impl_->device->getDescription().name; }

}  // namespace dw3
