// GPU renderer on plume. See hw_renderer.h for the overall design.

#include "dw3/hw_renderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "dw3/vertex_builder.h"
#include "dw3/vram_regions.h"
#include "plume_render_interface.h"
#include "shaders/present.frag.h"
#include "shaders/present.vert.h"
#include "shaders/psx_draw.frag.h"
#include "shaders/psx_draw.vert.h"

namespace plume {
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
extern void SetVulkanExtraInstanceExtensions(const std::vector<std::string>& extensions);  // from the plume patch
}

namespace dw3 {

using namespace plume;

namespace {

// Internal resolution multiplier. The render target, viewport, scissors and the pixel shader's coordinate
// mapping are all expressed through it; only 1 is supported by the VRAM copies for now.
constexpr int kScale = 1;

constexpr RenderFormat kVramFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr RenderFormat kSwapFormat = RenderFormat::B8G8R8A8_UNORM;

constexpr std::size_t kVertexBufferBytes = 48u << 20;
constexpr std::size_t kStagingBytes = 16u << 20;
constexpr std::size_t kReadbackBytes = 8u << 20;
constexpr std::size_t kMaxPendingVertices = 200000;

struct PresentParams {
    float dest[4];
    float window[2];
    float pad0[2];
    int32_t src[4];
    int32_t color24;
    int32_t linear;
};
static_assert(sizeof(PresentParams) == 56);

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
    std::unique_ptr<RenderCommandList> cmd;
    std::unique_ptr<RenderCommandFence> fence;
    bool recording = false;
    HwStats stats;

    // VRAM: `rt` is drawn into; `tex` is the sampled copy.
    std::unique_ptr<RenderTexture> rt, tex;
    std::unique_ptr<RenderFramebuffer> rt_fb;
    RenderTextureLayout rt_layout = RenderTextureLayout::UNKNOWN, tex_layout = RenderTextureLayout::UNKNOWN;

    std::unique_ptr<RenderShader> draw_vs, draw_fs, present_vs, present_fs;
    std::unique_ptr<RenderPipelineLayout> draw_layout, present_layout;
    std::unique_ptr<RenderPipeline> draw_pipeline, present_pipeline;
    std::unique_ptr<RenderDescriptorSet> draw_set, present_set;
    RenderInputSlot input_slot;
    std::vector<RenderInputElement> input_elements;

    std::unique_ptr<RenderBuffer> vertex_buf, staging_buf, readback_buf;
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
    std::unique_ptr<RenderCommandSemaphore> acquire_sem;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> release_sems;
    std::vector<std::unique_ptr<RenderFramebuffer>> swap_fbs;
    std::unique_ptr<RenderTexture> capture_tex;
    std::unique_ptr<RenderFramebuffer> capture_fb;
    std::unique_ptr<RenderBuffer> capture_buf;
    RenderTextureLayout capture_layout = RenderTextureLayout::UNKNOWN;
    uint32_t capture_w = 0, capture_h = 0;

    // ---- setup -------------------------------------------------------------------------------------

    bool init(const NativeWindow* window, std::string& error);
    bool create_pipelines();
    bool create_swapchain(const NativeWindow& window);
    void create_swap_framebuffers();
    void clear_vram();

    // ---- command recording --------------------------------------------------------------------------

    void ensure_recording() {
        if (!recording) {
            cmd->begin();
            recording = true;
        }
    }
    void transition(RenderTexture* t, RenderTextureLayout& cur, RenderTextureLayout want, bool force = false) {
        if (cur == want && !force) return;
        cmd->barriers(RenderBarrierStage::ALL, RenderTextureBarrier(t, want));
        cur = want;
    }
    void submit(RenderCommandSemaphore* wait = nullptr, RenderCommandSemaphore* signal = nullptr);
    void record_draws();
    void sync_copy();
    std::size_t staging_alloc(std::size_t bytes);

    // ---- primitives ---------------------------------------------------------------------------------

    void queue_primitive(const DrawState& s, const Region& bounds, bool reads_dest, const Region* tex_src,
                         const Region* clut_src, std::size_t vertices_before);
    void upload(std::span<const uint16_t> vram, const Region& r);
    void readback(const std::vector<Region>& regions, std::span<uint16_t> vram);

    // ---- presentation -------------------------------------------------------------------------------

    void draw_display(RenderFramebuffer* fb, uint32_t w, uint32_t h, const DisplayState& d, ScaleMode mode, bool linear);
    bool ensure_capture(uint32_t w, uint32_t h);
};

// -------------------------------------------------------------------------------------------------------
// Setup

bool HwRenderer::Impl::init(const NativeWindow* window, std::string& error) {
    iface = CreateVulkanInterface();
    if (!iface) {
        error = "Vulkan is not available";
        return false;
    }
    device = iface->createDevice();
    if (!device) {
        error = "no usable Vulkan device";
        return false;
    }
    queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    cmd = queue->createCommandList();
    fence = device->createCommandFence();
    if (!queue || !cmd || !fence) {
        error = "Vulkan queue creation failed";
        return false;
    }

    rt = device->createTexture(RenderTextureDesc::ColorTarget(1024 * kScale, 512 * kScale, kVramFormat));
    tex = device->createTexture(RenderTextureDesc::Texture2D(1024 * kScale, 512 * kScale, 1, kVramFormat));
    if (!rt || !tex) {
        error = "VRAM texture creation failed";
        return false;
    }
    const RenderTexture* attachment = rt.get();
    RenderFramebufferDesc fbd;
    fbd.colorAttachments = &attachment;
    fbd.colorAttachmentsCount = 1;
    rt_fb = device->createFramebuffer(fbd);

    vertex_buf = device->createBuffer(RenderBufferDesc::VertexBuffer(kVertexBufferBytes, RenderHeapType::UPLOAD));
    staging_buf = device->createBuffer(RenderBufferDesc::UploadBuffer(kStagingBytes));
    readback_buf = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kReadbackBytes));
    if (!vertex_buf || !staging_buf || !readback_buf || !rt_fb) {
        error = "buffer creation failed";
        return false;
    }
    vertex_map = static_cast<uint8_t*>(vertex_buf->map());
    staging_map = static_cast<uint8_t*>(staging_buf->map());
    if (!vertex_map || !staging_map) {
        error = "buffer mapping failed";
        return false;
    }

    if (!create_pipelines()) {
        error = "pipeline creation failed";
        return false;
    }
    if (window && !create_swapchain(*window)) {
        error = "swap chain creation failed";
        return false;
    }
    clear_vram();
    pending.reserve(kMaxPendingVertices + 8192);
    return true;
}

bool HwRenderer::Impl::create_pipelines() {
    const RenderShaderFormat fmt = iface->getCapabilities().shaderFormat;
    if (fmt != RenderShaderFormat::SPIRV) return false;
    draw_vs = device->createShader(shaders::psx_draw_vert_spv, shaders::psx_draw_vert_spv_size, "main", fmt);
    draw_fs = device->createShader(shaders::psx_draw_frag_spv, shaders::psx_draw_frag_spv_size, "main", fmt);
    present_vs = device->createShader(shaders::present_vert_spv, shaders::present_vert_spv_size, "main", fmt);
    present_fs = device->createShader(shaders::present_frag_spv, shaders::present_frag_spv_size, "main", fmt);
    if (!draw_vs || !draw_fs || !present_vs || !present_fs) return false;

    const RenderDescriptorRange range(RenderDescriptorRangeType::TEXTURE, 0, 1);
    const RenderDescriptorSetDesc set_desc(&range, 1);
    draw_set = device->createDescriptorSet(set_desc);
    present_set = device->createDescriptorSet(set_desc);
    draw_set->setTexture(0, tex.get(), RenderTextureLayout::SHADER_READ);
    present_set->setTexture(0, rt.get(), RenderTextureLayout::SHADER_READ);

    {
        const RenderPushConstantRange pc(0, 0, 0, 16, RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &set_desc;
        ld.descriptorSetDescsCount = 1;
        ld.allowInputLayout = true;
        draw_layout = device->createPipelineLayout(ld);

        input_slot = RenderInputSlot(0, sizeof(HwVertex));
        input_elements = {
            RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(HwVertex, x)),
            RenderInputElement("COLOR", 0, 1, RenderFormat::R8G8B8A8_UNORM, 0, offsetof(HwVertex, r)),
            RenderInputElement("TEXCOORD", 0, 2, RenderFormat::R32G32_FLOAT, 0, offsetof(HwVertex, u)),
            RenderInputElement("STATE", 0, 3, RenderFormat::R32G32B32_UINT, 0, offsetof(HwVertex, state0)),
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
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        draw_pipeline = device->createGraphicsPipeline(pd);
    }
    {
        const RenderPushConstantRange pc(0, 0, 0, sizeof(PresentParams),
                                         RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &set_desc;
        ld.descriptorSetDescsCount = 1;
        present_layout = device->createPipelineLayout(ld);

        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = present_layout.get();
        pd.vertexShader = present_vs.get();
        pd.pixelShader = present_fs.get();
        pd.renderTargetFormat[0] = kSwapFormat;
        pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pd.renderTargetCount = 1;
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        present_pipeline = device->createGraphicsPipeline(pd);
    }
    return draw_layout && draw_pipeline && present_layout && present_pipeline;
}

bool HwRenderer::Impl::create_swapchain(const NativeWindow& window) {
    RenderWindow w{};
    w.userData = window.user;
    w.createSurface = window.create_surface;
    w.getSize = window.get_size;
    swap = queue->createSwapChain(RenderSwapChainDesc(w, kSwapFormat, 2));
    if (!swap || !swap->resize()) return false;
    acquire_sem = device->createCommandSemaphore();
    create_swap_framebuffers();
    return !swap_fbs.empty();
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
    cmd->setFramebuffer(rt_fb.get());
    cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    transition(rt.get(), rt_layout, RenderTextureLayout::COPY_SOURCE);
    transition(tex.get(), tex_layout, RenderTextureLayout::COPY_DEST);
    cmd->copyTexture(tex.get(), rt.get());
    submit();
}

// -------------------------------------------------------------------------------------------------------
// Submission and buffers

void HwRenderer::Impl::submit(RenderCommandSemaphore* wait, RenderCommandSemaphore* signal) {
    ensure_recording();
    cmd->end();
    recording = false;
    const RenderCommandList* list = cmd.get();
    RenderCommandSemaphore* waits[1] = {wait};
    RenderCommandSemaphore* signals[1] = {signal};
    queue->executeCommandLists(&list, 1, wait ? waits : nullptr, wait ? 1 : 0, signal ? signals : nullptr,
                               signal ? 1 : 0, fence.get());
    queue->waitForCommandFence(fence.get());
    vertex_used = 0;
    staging_used = 0;
    ++stats.submits;
}

std::size_t HwRenderer::Impl::staging_alloc(std::size_t bytes) {
    assert(bytes <= kStagingBytes);
    if (staging_used + bytes > kStagingBytes) {
        record_draws();
        submit();
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
    cmd->setFramebuffer(rt_fb.get());
    cmd->setViewports(RenderViewport(0.0f, 0.0f, 1024.0f * kScale, 512.0f * kScale));
    cmd->setGraphicsPipelineLayout(draw_layout.get());
    cmd->setPipeline(draw_pipeline.get());
    cmd->setGraphicsDescriptorSet(draw_set.get(), 0);
    const int32_t scale[4] = {kScale, 0, 0, 0};
    cmd->setGraphicsPushConstants(0, scale);
    const RenderVertexBufferView view(vertex_buf->at(0), static_cast<uint32_t>(kVertexBufferBytes));
    cmd->setVertexBuffers(0, &view, 1, &input_slot);
    for (const DrawRange& r : ranges) {
        cmd->setScissors(RenderRect(r.scissor.x0 * kScale, r.scissor.y0 * kScale, r.scissor.x1 * kScale,
                                    r.scissor.y1 * kScale));
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
    for (const Region& r : stale.take_all()) {
        const RenderBox box(r.x0 * kScale, r.y0 * kScale, r.x1 * kScale, r.y1 * kScale);
        cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(tex.get()),
                               RenderTextureCopyLocation::Subresource(rt.get()), r.x0 * kScale, r.y0 * kScale, 0, &box);
    }
    ++stats.copy_syncs;
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
    dirty.add(bounds);
    if (pending.size() >= kMaxPendingVertices) record_draws();
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
    transition(rt.get(), rt_layout, RenderTextureLayout::COPY_DEST, true);
    cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(rt.get()),
                           RenderTextureCopyLocation::PlacedFootprint(staging_buf.get(), kVramFormat, w, h, 1, pitch, off),
                           r.x0 * kScale, r.y0 * kScale, 0);
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
    transition(rt.get(), rt_layout, RenderTextureLayout::COPY_SOURCE);
    std::size_t off = 0;
    std::vector<std::size_t> offsets;
    for (const Region& r : regions) {
        const uint32_t w = static_cast<uint32_t>(r.width()), h = static_cast<uint32_t>(r.height());
        const uint32_t pitch = upload_row_pixels(w);
        const RenderBox box(r.x0 * kScale, r.y0 * kScale, r.x1 * kScale, r.y1 * kScale);
        cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback_buf.get(), kVramFormat, w, h, 1, pitch, off),
                               RenderTextureCopyLocation::Subresource(rt.get()), 0, 0, 0, &box);
        offsets.push_back(off);
        off += align_up(static_cast<std::size_t>(pitch) * h * 4, 512);
    }
    submit();
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
// Presentation

void HwRenderer::Impl::draw_display(RenderFramebuffer* fb, uint32_t w, uint32_t h, const DisplayState& d, ScaleMode mode, bool linear) {
    cmd->setFramebuffer(fb);
    cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
    cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)));
    cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    if (!d.enabled) return;
    const PresentRect pr = compute_present_rect(d.width, d.height, static_cast<int>(w), static_cast<int>(h), mode);
    if (pr.w <= 0 || pr.h <= 0) return;
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
    cmd->setGraphicsPipelineLayout(present_layout.get());
    cmd->setPipeline(present_pipeline.get());
    cmd->setGraphicsDescriptorSet(present_set.get(), 0);
    cmd->setGraphicsPushConstants(0, &p);
    cmd->drawInstanced(6, 1, 0, 0);
    ++stats.draw_calls;
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
        if (impl_->vertex_map) impl_->vertex_buf->unmap();
        if (impl_->staging_map) impl_->staging_buf->unmap();
    }
}

std::unique_ptr<HwRenderer> HwRenderer::create(const NativeWindow* window, const std::vector<std::string>& instance_extensions,
                                               std::string& error) {
    SetVulkanExtraInstanceExtensions(instance_extensions);
    std::unique_ptr<HwRenderer> r(new HwRenderer());
    if (!r->impl_->init(window, error)) return nullptr;
    return r;
}

void HwRenderer::triangle(std::span<uint16_t>, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) {
    Impl& m = *impl_;
    const Region bounds = triangle_bounds(s, v);
    if (bounds.empty()) return;
    const std::size_t before = m.pending.size();
    build_triangle(m.pending, s, f, v);
    const bool tex = uses_texture(s, f);
    Region t, c;
    if (tex) {
        t = triangle_texture_region(s, v);
        c = clut_region(s, f);
    }
    m.queue_primitive(s, bounds, f.semi_transparent || s.check_mask, tex ? &t : nullptr, tex ? &c : nullptr, before);
}

void HwRenderer::rect(std::span<uint16_t>, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                      int height) {
    Impl& m = *impl_;
    const Region bounds = rect_bounds(s, v, width, height);
    if (bounds.empty()) return;
    const std::size_t before = m.pending.size();
    build_rect(m.pending, s, f, v, width, height);
    const bool tex = uses_texture(s, f);
    Region t, c;
    if (tex) {
        t = rect_texture_region(s, v, width, height);
        c = clut_region(s, f);
    }
    m.queue_primitive(s, bounds, f.semi_transparent || s.check_mask, tex ? &t : nullptr, tex ? &c : nullptr, before);
}

void HwRenderer::line(std::span<uint16_t>, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
                      const GpuVertex& b) {
    Impl& m = *impl_;
    const Region bounds = line_bounds(s, a, b);
    if (bounds.empty()) return;
    const std::size_t before = m.pending.size();
    if (build_line(m.pending, s, f, a, b) == 0) return;
    m.queue_primitive(s, bounds, f.semi_transparent || s.check_mask, nullptr, nullptr, before);
}

void HwRenderer::vram_written(std::span<const uint16_t> vram, const VramRect& r) {
    impl_->upload(vram, region_from(r));
}

void HwRenderer::vram_read_begin(std::span<uint16_t> vram, const VramRect& r) {
    Impl& m = *impl_;
    const Region want = region_from(r);
    if (!m.dirty.intersects(want)) return;
    m.readback(m.dirty.take_intersecting(want), vram);
}

void HwRenderer::read_vram(std::span<uint16_t> out) {
    impl_->readback({kWholeVram}, out);
    impl_->dirty.clear();
}

bool HwRenderer::present(const DisplayState& display, ScaleMode mode, bool linear, std::vector<uint32_t>* capture,
                         int* capture_w, int* capture_h) {
    Impl& m = *impl_;
    if (!m.swap) return false;
    m.record_draws();
    if (m.swap->needsResize()) {
        m.swap_fbs.clear();
        if (!m.swap->resize()) return false;
        m.create_swap_framebuffers();
    }
    if (m.swap->isEmpty() || m.swap_fbs.empty()) {
        m.submit();
        return false;
    }
    uint32_t index = 0;
    if (!m.swap->acquireTexture(m.acquire_sem.get(), &index)) {
        // Out of date: rebuild on the next frame. The acquire semaphore was not signalled.
        m.submit();
        m.swap_fbs.clear();
        if (m.swap->resize()) m.create_swap_framebuffers();
        return false;
    }
    const uint32_t w = m.swap->getWidth(), h = m.swap->getHeight();
    m.ensure_recording();
    m.transition(m.rt.get(), m.rt_layout, RenderTextureLayout::SHADER_READ);
    RenderTexture* target = m.swap->getTexture(index);
    m.cmd->barriers(RenderBarrierStage::ALL, RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
    m.draw_display(m.swap_fbs[index].get(), w, h, display, mode, linear);

    const bool grab = capture != nullptr && m.ensure_capture(w, h);
    if (grab) {
        m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COLOR_WRITE);
        m.draw_display(m.capture_fb.get(), w, h, display, mode, linear);
        m.transition(m.capture_tex.get(), m.capture_layout, RenderTextureLayout::COPY_SOURCE);
        const uint32_t pitch = upload_row_pixels(w);
        m.cmd->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(m.capture_buf.get(), kSwapFormat, w, h, 1, pitch, 0),
            RenderTextureCopyLocation::Subresource(m.capture_tex.get()), 0, 0, 0);
    }
    m.cmd->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
    RenderCommandSemaphore* release = m.release_sems[index].get();
    m.submit(m.acquire_sem.get(), release);
    RenderCommandSemaphore* waits[1] = {release};
    m.swap->present(index, waits, 1);

    if (grab) {
        const uint32_t pitch = upload_row_pixels(w);
        const auto* data = static_cast<const uint32_t*>(m.capture_buf->map());
        capture->resize(static_cast<std::size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t bgra = data[static_cast<std::size_t>(y) * pitch + x];
                (*capture)[static_cast<std::size_t>(y) * w + x] =
                    (bgra & 0xFF00FF00u) | ((bgra & 0xFF) << 16) | ((bgra >> 16) & 0xFF) | 0xFF000000u;
            }
        }
        m.capture_buf->unmap();
        if (capture_w) *capture_w = static_cast<int>(w);
        if (capture_h) *capture_h = static_cast<int>(h);
    }
    return true;
}

void HwRenderer::set_vsync(bool enabled) {
    if (impl_->swap) impl_->swap->setVsyncEnabled(enabled);
}

const HwStats& HwRenderer::stats() const { return impl_->stats; }

std::string HwRenderer::device_name() const { return impl_->device->getDescription().name; }

}  // namespace dw3
