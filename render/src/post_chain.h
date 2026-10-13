#pragma once

// The post-processing passes of the GPU renderer (shaders in render/shaders/post). The renderer draws the picture into
// source(); run() applies the enabled passes and returns how the last one is to be drawn into the window, and draw()
// does that into a framebuffer (the swap chain image, or the capture target). See docs/ARCHITECTURE.md.

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>

#include "dw3/postfx.h"
#include "plume_render_interface.h"

namespace dw3 {

class PostChain {
public:
    // An image the passes draw into and read from. All are the swap chain's colour format.
    struct Target {
        std::unique_ptr<plume::RenderTexture> tex;
        std::unique_ptr<plume::RenderFramebuffer> fb;
        std::unique_ptr<plume::RenderDescriptorSet> set;  // binds the texture alone
        plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
        uint32_t w = 0, h = 0;
    };

    // The last pass, left for draw() so that it can be repeated for several framebuffers.
    struct Plan {
        enum class Kind { Scale, Rcas, Crt } kind = Kind::Scale;
        Target* input = nullptr;
        bool linear = false;
        float rcas_stops = 0.0f;
        CrtParams crt;
        float lines = 240.0f;  // the console's lines in the picture (for the scanlines)
        HdrParams hdr;         // output transform of the last pass (mode Off for an SDR swap chain)
    };

    // `target_format` is the swap chain's colour format; the passes' images use it too, so one pipeline serves all.
    PostChain(plume::RenderDevice* device, plume::RenderShaderFormat format, plume::RenderFormat target_format);
    ~PostChain();

    // Makes the targets for a picture of src_w x src_h pixels shown in a pic_w x pic_h rectangle of the window, and uploads
    // the SMAA lookup textures the first time they are needed. `wait_idle` is called before any target that earlier
    // frames may still use is replaced. Must be called outside a render pass. Returns false if a resource could not
    // be created.
    bool prepare(plume::RenderCommandList* cmd, const PostFx& fx, uint32_t src_w, uint32_t src_h, uint32_t pic_w,
                 uint32_t pic_h, const std::function<void()>& wait_idle);

    // The target the picture is drawn into (valid after prepare), made ready for drawing.
    Target& source(plume::RenderCommandList* cmd);

    // Runs the passes that come before the final one. Returns false if a pipeline could not be created.
    bool run(plume::RenderCommandList* cmd, const PostFx& fx, uint32_t pic_w, uint32_t pic_h, float lines, Plan& plan);

    // The final pass: draws the plan's result into the rectangle of `fb`, which is win_w x win_h pixels. `fb` must be bound
    // for drawing already; the caller has cleared it.
    bool draw(plume::RenderCommandList* cmd, const Plan& plan, plume::RenderFramebuffer* fb, uint32_t win_w, uint32_t win_h,
              int x, int y, int w, int h);

private:
    enum PassId { Scale, Cas, Grade, Crt, Easu, Rcas, SmaaEdges, SmaaWeights, SmaaBlend, PassCount };
    struct Pass {
        std::unique_ptr<plume::RenderShader> fs;
        std::unique_ptr<plume::RenderPipeline> pipeline;
        int textures = 1;
    };

    bool ensure_target(Target& t, uint32_t w, uint32_t h, const std::function<void()>& wait_idle, bool& ok);
    bool pipeline_for(PassId id);
    void transition(plume::RenderCommandList* cmd, plume::RenderTexture* tex, plume::RenderTextureLayout& cur,
                    plume::RenderTextureLayout want);
    void to_read(plume::RenderCommandList* cmd, Target& t);
    bool draw_pass(plume::RenderCommandList* cmd, PassId id, plume::RenderFramebuffer* fb, uint32_t win_w, uint32_t win_h,
                   float dx, float dy, float dw, float dh, plume::RenderDescriptorSet* set, uint32_t src_w, uint32_t src_h,
                   const float a[4], const float b[4], const float c[4] = nullptr);
    // Draws pass `id` over the whole of `out`; the inputs are made readable first.
    bool pass_to_target(plume::RenderCommandList* cmd, PassId id, Target& out, plume::RenderDescriptorSet* set,
                        std::initializer_list<Target*> inputs, uint32_t src_w, uint32_t src_h, const float a[4],
                        const float b[4]);
    bool upload_lookups(plume::RenderCommandList* cmd);
    void build_smaa_sets();

    plume::RenderDevice* device_;
    plume::RenderShaderFormat format_;
    plume::RenderFormat target_format_;
    std::unique_ptr<plume::RenderShader> vs_;
    std::unique_ptr<plume::RenderPipelineLayout> layouts_[4];  // by number of input textures
    Pass passes_[PassCount];
    // Targets: a ping-pong pair at the picture's resolution, the SMAA edge and weight images, and the window-sized pair
    // of FSR.
    Target a_, b_, edges_, weights_, win_a_, win_b_;
    std::unique_ptr<plume::RenderTexture> area_, search_;
    plume::RenderTextureLayout area_layout_ = plume::RenderTextureLayout::UNKNOWN;
    plume::RenderTextureLayout search_layout_ = plume::RenderTextureLayout::UNKNOWN;
    std::unique_ptr<plume::RenderBuffer> lookup_upload_;
    std::unique_ptr<plume::RenderDescriptorSet> weights_set_, blend_set_;
    bool lookups_ready_ = false;
};

}  // namespace dw3
