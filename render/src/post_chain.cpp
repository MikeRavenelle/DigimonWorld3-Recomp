#include "post_chain.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "dw3/present_layout.h"
#include "shaders/post.vert.h"
#include "shaders/post_cas.frag.h"
#include "shaders/post_crt.frag.h"
#include "shaders/post_easu.frag.h"
#include "shaders/post_grade.frag.h"
#include "shaders/post_rcas.frag.h"
#include "shaders/post_scale.frag.h"
#include "shaders/post_smaa_blend.frag.h"
#include "shaders/post_smaa_edges.frag.h"
#include "shaders/post_smaa_weights.frag.h"
#include "shaders/smaa_area.h"
#include "shaders/smaa_search.h"
#ifdef DW3_HAVE_METAL
#include "shaders/post.vert.msl.h"
#include "shaders/post_cas.frag.msl.h"
#include "shaders/post_crt.frag.msl.h"
#include "shaders/post_easu.frag.msl.h"
#include "shaders/post_grade.frag.msl.h"
#include "shaders/post_rcas.frag.msl.h"
#include "shaders/post_scale.frag.msl.h"
#include "shaders/post_smaa_blend.frag.msl.h"
#include "shaders/post_smaa_edges.frag.msl.h"
#include "shaders/post_smaa_weights.frag.msl.h"
#endif
#ifdef DW3_HAVE_D3D12
#include "shaders/post.vert.dxil.h"
#include "shaders/post_cas.frag.dxil.h"
#include "shaders/post_crt.frag.dxil.h"
#include "shaders/post_easu.frag.dxil.h"
#include "shaders/post_grade.frag.dxil.h"
#include "shaders/post_rcas.frag.dxil.h"
#include "shaders/post_scale.frag.dxil.h"
#include "shaders/post_smaa_blend.frag.dxil.h"
#include "shaders/post_smaa_edges.frag.dxil.h"
#include "shaders/post_smaa_weights.frag.dxil.h"
#endif

namespace dw3 {

using namespace plume;

namespace {

constexpr RenderFormat kLookupFormat = RenderFormat::R8G8B8A8_UNORM;

constexpr uint32_t kAreaW = 160, kAreaH = 560, kSearchW = 64, kSearchH = 16;

// Push constants shared by every pass (post/common.glsl).
struct PassConstants {
    float dest[4];
    float window[2];
    float src[2];
    float a[4];
    float b[4];
    float c[4];  // HDR output of the last pass, zero otherwise
};
static_assert(sizeof(PassConstants) == 80);

struct Blob {
    const uint8_t* data;
    size_t size;
};

#ifdef DW3_HAVE_D3D12
#define DW3_POST_DXIL(name) Blob{shaders::name##_dxil, shaders::name##_dxil_size}
#else
#define DW3_POST_DXIL(name) Blob{nullptr, 0}
#endif
#ifdef DW3_HAVE_METAL
#define DW3_POST_MSL(name) Blob{shaders::name##_msl, shaders::name##_msl_size}
#else
#define DW3_POST_MSL(name) Blob{nullptr, 0}
#endif

struct ShaderBlobs {
    Blob spv, dxil, msl;
};
#define DW3_POST_BLOBS(name) ShaderBlobs{Blob{shaders::name##_spv, shaders::name##_spv_size}, DW3_POST_DXIL(name), DW3_POST_MSL(name)}

std::unique_ptr<RenderShader> make_shader(RenderDevice* device, RenderShaderFormat fmt, const ShaderBlobs& b) {
    if (fmt == RenderShaderFormat::SPIRV) return device->createShader(b.spv.data, b.spv.size, "main", fmt);
    if (fmt == RenderShaderFormat::DXIL && b.dxil.data) return device->createShader(b.dxil.data, b.dxil.size, "main", fmt);
    // SPIRV-Cross names the entry point main0, since main is reserved in Metal.
    if (fmt == RenderShaderFormat::METAL && b.msl.data) return device->createShader(b.msl.data, b.msl.size, "main0", fmt);
    return nullptr;
}

std::unique_ptr<RenderBuffer> make_upload_buffer(RenderDevice* device, uint64_t bytes) {
    return device->createBuffer(RenderBufferDesc::UploadBuffer(bytes));
}

}  // namespace

PostChain::PostChain(RenderDevice* device, RenderShaderFormat format, RenderFormat target_format)
    : device_(device), format_(format), target_format_(target_format) {}
PostChain::~PostChain() = default;

bool PostChain::ensure_target(Target& t, uint32_t w, uint32_t h, const std::function<void()>& wait_idle, bool& ok) {
    if (t.tex && t.w == w && t.h == h) return true;
    if (t.tex) wait_idle();  // earlier frames may still read it
    t.fb.reset();
    t.set.reset();
    t.tex = device_->createTexture(RenderTextureDesc::ColorTarget(w, h, target_format_));
    t.layout = RenderTextureLayout::UNKNOWN;
    t.w = w;
    t.h = h;
    if (!t.tex) return ok = false;
    const RenderTexture* attachment = t.tex.get();
    RenderFramebufferDesc fbd;
    fbd.colorAttachments = &attachment;
    fbd.colorAttachmentsCount = 1;
    t.fb = device_->createFramebuffer(fbd);
    const RenderDescriptorRange range(RenderDescriptorRangeType::TEXTURE, 0, 1);
    t.set = device_->createDescriptorSet(RenderDescriptorSetDesc(&range, 1));
    if (!t.fb || !t.set) return ok = false;
    t.set->setTexture(0, t.tex.get(), RenderTextureLayout::SHADER_READ);
    // Sets that bind this target alongside others are rebuilt by prepare().
    if (&t == &a_ || &t == &edges_ || &t == &weights_) {
        weights_set_.reset();
        blend_set_.reset();
    }
    return true;
}

void PostChain::transition(RenderCommandList* cmd, RenderTexture* tex, RenderTextureLayout& cur, RenderTextureLayout want) {
    if (cur == want) return;
    cmd->barriers(RenderBarrierStage::ALL, RenderTextureBarrier(tex, want));
    cur = want;
}

bool PostChain::upload_lookups(RenderCommandList* cmd) {
    if (lookups_ready_) return true;
    const uint32_t area_pitch = upload_row_pixels(kAreaW), search_pitch = upload_row_pixels(kSearchW);
    const uint64_t area_bytes = static_cast<uint64_t>(area_pitch) * kAreaH * 4;
    const uint64_t search_off = (area_bytes + 511) / 512 * 512;
    lookup_upload_ = make_upload_buffer(device_, search_off + static_cast<uint64_t>(search_pitch) * kSearchH * 4);
    area_ = device_->createTexture(RenderTextureDesc::Texture2D(kAreaW, kAreaH, 1, kLookupFormat));
    search_ = device_->createTexture(RenderTextureDesc::Texture2D(kSearchW, kSearchH, 1, kLookupFormat));
    if (!lookup_upload_ || !area_ || !search_) return false;
    auto* base = static_cast<uint8_t*>(lookup_upload_->map());
    if (!base) return false;
    auto* area = reinterpret_cast<uint32_t*>(base);
    for (uint32_t y = 0; y < kAreaH; ++y)
        for (uint32_t x = 0; x < kAreaW; ++x) {
            const uint8_t* p = shaders::smaa_area + (static_cast<size_t>(y) * kAreaW + x) * 2;
            area[static_cast<size_t>(y) * area_pitch + x] = p[0] | (static_cast<uint32_t>(p[1]) << 8) | 0xFF000000u;
        }
    auto* search = reinterpret_cast<uint32_t*>(base + search_off);
    for (uint32_t y = 0; y < kSearchH; ++y)
        for (uint32_t x = 0; x < kSearchW; ++x)
            search[static_cast<size_t>(y) * search_pitch + x] = shaders::smaa_search[y * kSearchW + x] | 0xFF000000u;
    lookup_upload_->unmap();

    transition(cmd, area_.get(), area_layout_, RenderTextureLayout::COPY_DEST);
    cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(area_.get()),
                           RenderTextureCopyLocation::PlacedFootprint(lookup_upload_.get(), kLookupFormat, kAreaW, kAreaH, 1,
                                                                      area_pitch, 0),
                           0, 0, 0);
    transition(cmd, area_.get(), area_layout_, RenderTextureLayout::SHADER_READ);
    transition(cmd, search_.get(), search_layout_, RenderTextureLayout::COPY_DEST);
    cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(search_.get()),
                           RenderTextureCopyLocation::PlacedFootprint(lookup_upload_.get(), kLookupFormat, kSearchW, kSearchH, 1,
                                                                      search_pitch, search_off),
                           0, 0, 0);
    transition(cmd, search_.get(), search_layout_, RenderTextureLayout::SHADER_READ);
    lookups_ready_ = true;
    return true;
}

bool PostChain::prepare(RenderCommandList* cmd, const PostFx& fx, uint32_t src_w, uint32_t src_h, uint32_t pic_w,
                        uint32_t pic_h, const std::function<void()>& wait_idle) {
    bool ok = true;
    ensure_target(a_, src_w, src_h, wait_idle, ok);
    const bool fsr = fx.scaler == OutputScaler::Fsr && (pic_w > src_w || pic_h > src_h);
    const bool cas = fx.sharpness > 0.0f && !fsr;
    if (fx.smaa || cas || !fx.grade.neutral()) ensure_target(b_, src_w, src_h, wait_idle, ok);
    if (fx.smaa) {
        ensure_target(edges_, src_w, src_h, wait_idle, ok);
        ensure_target(weights_, src_w, src_h, wait_idle, ok);
        if (ok && !upload_lookups(cmd)) ok = false;
    } else if (edges_.tex || weights_.tex) {
        wait_idle();
        edges_ = Target();
        weights_ = Target();
        weights_set_.reset();
        blend_set_.reset();
    }
    if (fsr) {
        ensure_target(win_a_, pic_w, pic_h, wait_idle, ok);
        if (fx.sharpness > 0.0f && fx.crt.enabled()) ensure_target(win_b_, pic_w, pic_h, wait_idle, ok);
    }
    if (ok && fx.smaa && (!weights_set_ || !blend_set_)) build_smaa_sets();
    return ok && (!fx.smaa || (weights_set_ && blend_set_));
}

void PostChain::build_smaa_sets() {
    const RenderDescriptorRange three[3] = {RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
                                            RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
                                            RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 2, 1)};
    weights_set_ = device_->createDescriptorSet(RenderDescriptorSetDesc(three, 3));
    blend_set_ = device_->createDescriptorSet(RenderDescriptorSetDesc(three, 2));
    if (!weights_set_ || !blend_set_) return;
    weights_set_->setTexture(0, edges_.tex.get(), RenderTextureLayout::SHADER_READ);
    weights_set_->setTexture(1, area_.get(), RenderTextureLayout::SHADER_READ);
    weights_set_->setTexture(2, search_.get(), RenderTextureLayout::SHADER_READ);
    blend_set_->setTexture(0, a_.tex.get(), RenderTextureLayout::SHADER_READ);
    blend_set_->setTexture(1, weights_.tex.get(), RenderTextureLayout::SHADER_READ);
}

PostChain::Target& PostChain::source(RenderCommandList* cmd) {
    transition(cmd, a_.tex.get(), a_.layout, RenderTextureLayout::COLOR_WRITE);
    return a_;
}

void PostChain::to_read(RenderCommandList* cmd, Target& t) { transition(cmd, t.tex.get(), t.layout, RenderTextureLayout::SHADER_READ); }

bool PostChain::pipeline_for(PassId id) {
    Pass& p = passes_[id];
    if (p.pipeline) return true;
    if (!vs_) vs_ = make_shader(device_, format_, DW3_POST_BLOBS(post_vert));
    ShaderBlobs blobs;
    int textures = 1;
    switch (id) {
        case Scale: blobs = DW3_POST_BLOBS(post_scale_frag); break;
        case Cas: blobs = DW3_POST_BLOBS(post_cas_frag); break;
        case Grade: blobs = DW3_POST_BLOBS(post_grade_frag); break;
        case Crt: blobs = DW3_POST_BLOBS(post_crt_frag); break;
        case Easu: blobs = DW3_POST_BLOBS(post_easu_frag); break;
        case Rcas: blobs = DW3_POST_BLOBS(post_rcas_frag); break;
        case SmaaEdges: blobs = DW3_POST_BLOBS(post_smaa_edges_frag); break;
        case SmaaWeights: blobs = DW3_POST_BLOBS(post_smaa_weights_frag); textures = 3; break;
        case SmaaBlend: blobs = DW3_POST_BLOBS(post_smaa_blend_frag); textures = 2; break;
        default: return false;
    }
    p.fs = make_shader(device_, format_, blobs);
    if (!vs_ || !p.fs) return false;
    p.textures = textures;
    if (!layouts_[textures]) {
        RenderDescriptorRange ranges[3];
        for (int i = 0; i < textures; ++i) ranges[i] = RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, i, 1);
        const RenderDescriptorSetDesc set_desc(ranges, textures);
        const RenderPushConstantRange pc(0, 0, 0, sizeof(PassConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        RenderPipelineLayoutDesc ld;
        ld.pushConstantRanges = &pc;
        ld.pushConstantRangesCount = 1;
        ld.descriptorSetDescs = &set_desc;
        ld.descriptorSetDescsCount = 1;
        layouts_[textures] = device_->createPipelineLayout(ld);
        if (!layouts_[textures]) return false;
    }
    RenderGraphicsPipelineDesc pd;
    pd.pipelineLayout = layouts_[textures].get();
    pd.vertexShader = vs_.get();
    pd.pixelShader = p.fs.get();
    pd.renderTargetFormat[0] = target_format_;
    pd.renderTargetBlend[0] = RenderBlendDesc::Copy();
    pd.renderTargetCount = 1;
    pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    p.pipeline = device_->createGraphicsPipeline(pd);
    return p.pipeline != nullptr;
}

bool PostChain::draw_pass(RenderCommandList* cmd, PassId id, RenderFramebuffer* fb, uint32_t win_w, uint32_t win_h, float dx,
                          float dy, float dw, float dh, RenderDescriptorSet* set, uint32_t src_w, uint32_t src_h, const float a[4],
                          const float b[4], const float hdr[4]) {
    if (!pipeline_for(id)) return false;
    PassConstants c{};
    c.dest[0] = dx;
    c.dest[1] = dy;
    c.dest[2] = dw;
    c.dest[3] = dh;
    c.window[0] = static_cast<float>(win_w);
    c.window[1] = static_cast<float>(win_h);
    c.src[0] = static_cast<float>(src_w);
    c.src[1] = static_cast<float>(src_h);
    std::memcpy(c.a, a, sizeof(c.a));
    std::memcpy(c.b, b, sizeof(c.b));
    if (hdr) std::memcpy(c.c, hdr, sizeof(c.c));
    if (fb) {
        cmd->setFramebuffer(fb);
        cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(win_w), static_cast<float>(win_h)));
        cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(win_w), static_cast<int32_t>(win_h)));
    }
    cmd->setGraphicsPipelineLayout(layouts_[passes_[id].textures].get());
    cmd->setPipeline(passes_[id].pipeline.get());
    cmd->setGraphicsDescriptorSet(set, 0);
    cmd->setGraphicsPushConstants(0, &c);
    cmd->drawInstanced(6, 1, 0, 0);
    return true;
}

bool PostChain::pass_to_target(RenderCommandList* cmd, PassId id, Target& out, RenderDescriptorSet* set,
                               std::initializer_list<Target*> inputs, uint32_t src_w, uint32_t src_h, const float a[4],
                               const float b[4]) {
    for (Target* t : inputs) to_read(cmd, *t);
    transition(cmd, out.tex.get(), out.layout, RenderTextureLayout::COLOR_WRITE);
    return draw_pass(cmd, id, out.fb.get(), out.w, out.h, 0.0f, 0.0f, static_cast<float>(out.w), static_cast<float>(out.h), set,
                     src_w, src_h, a, b);
}

bool PostChain::run(RenderCommandList* cmd, const PostFx& fx, uint32_t pic_w, uint32_t pic_h, float lines, Plan& plan) {
    static const float kNone[4] = {};
    Target* cur = &a_;
    Target* other = &b_;
    const bool fsr = fx.scaler == OutputScaler::Fsr && (pic_w > a_.w || pic_h > a_.h);
    bool ok = true;

    if (fx.smaa) {
        ok = ok && pass_to_target(cmd, SmaaEdges, edges_, a_.set.get(), {&a_}, a_.w, a_.h, kNone, kNone);
        // The weights set holds the lookup textures, which stay readable once uploaded.
        ok = ok && pass_to_target(cmd, SmaaWeights, weights_, weights_set_.get(), {&edges_}, a_.w, a_.h, kNone, kNone);
        ok = ok && pass_to_target(cmd, SmaaBlend, b_, blend_set_.get(), {&a_, &weights_}, a_.w, a_.h, kNone, kNone);
        std::swap(cur, other);
    }
    if (fx.sharpness > 0.0f && !fsr) {
        const float a[4] = {fx.sharpness, 0, 0, 0};
        ok = ok && pass_to_target(cmd, Cas, *other, cur->set.get(), {cur}, cur->w, cur->h, a, kNone);
        std::swap(cur, other);
    }
    if (!fx.grade.neutral()) {
        const GradeParams& g = fx.grade;
        const float a[4] = {g.saturation, g.contrast, g.vibrance, g.gamma};
        const float b[4] = {g.tint[0], g.tint[1], g.tint[2], g.brightness};
        ok = ok && pass_to_target(cmd, Grade, *other, cur->set.get(), {cur}, cur->w, cur->h, a, b);
        std::swap(cur, other);
    }

    plan = Plan();
    plan.crt = fx.crt;
    plan.lines = lines;
    plan.linear = fx.scaler != OutputScaler::Nearest;
    plan.hdr = fx.hdr;
    if (fsr) {
        const float stops = 2.0f * (1.0f - std::clamp(fx.sharpness, 0.0f, 1.0f));
        ok = ok && pass_to_target(cmd, Easu, win_a_, cur->set.get(), {cur}, cur->w, cur->h, kNone, kNone);
        cur = &win_a_;
        plan.linear = false;  // 1:1 from here on
        if (fx.sharpness > 0.0f) {
            plan.rcas_stops = stops;
            if (fx.crt.enabled()) {
                const float a[4] = {stops, 0, 0, 0};
                ok = ok && pass_to_target(cmd, Rcas, win_b_, cur->set.get(), {cur}, cur->w, cur->h, a, kNone);
                cur = &win_b_;
            } else {
                plan.kind = Plan::Kind::Rcas;
            }
        }
    }
    if (fx.crt.enabled()) plan.kind = Plan::Kind::Crt;
    plan.input = cur;
    return ok;
}

bool PostChain::draw(RenderCommandList* cmd, const Plan& plan, RenderFramebuffer* fb, uint32_t win_w, uint32_t win_h, int x, int y,
                     int w, int h) {
    to_read(cmd, *plan.input);
    float a[4] = {}, b[4] = {};
    PassId id = Scale;
    switch (plan.kind) {
        case Plan::Kind::Scale: a[0] = plan.linear ? 1.0f : 0.0f; break;
        case Plan::Kind::Rcas: id = Rcas; a[0] = plan.rcas_stops; break;
        case Plan::Kind::Crt:
            id = Crt;
            a[0] = plan.crt.scanlines;
            a[1] = plan.crt.mask;
            a[2] = plan.crt.curvature;
            a[3] = plan.crt.vignette;
            b[1] = plan.lines;
            break;
    }
    // The output code of hdr.glsl: 0 SDR, 1 scRGB, 2 PQ, 3 EDR.
    const float hdr[4] = {static_cast<float>(plan.hdr.mode), plan.hdr.peak_nits, plan.hdr.paper_white_nits, 0.0f};
    return draw_pass(cmd, id, fb, win_w, win_h, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                     static_cast<float>(h), plan.input->set.get(), plan.input->w, plan.input->h, a, b, hdr);
}

}  // namespace dw3
