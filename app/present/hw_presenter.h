#pragma once

#include "dw3/hw_renderer.h"
#include "presenter.h"

namespace dw3app {

// Presentation through the plume GPU renderer: the emulated GPU draws into a VRAM texture and the display
// area is scaled into the window straight from it.
class HwPresenter final : public Presenter {
public:
    static std::unique_ptr<HwPresenter> create(SDL_Window* window, dw3::GraphicsApi api, const PresentOptions& options, std::string& error);
    ~HwPresenter() override;

    const char* name() const override { return name_.c_str(); }
    void set_vsync(VsyncMode mode) override;
    bool supports_hdr() const override;
    bool update_options(const PresentOptions& options, dw3::Gpu& gpu) override;
    void set_wide_mode(dw3::WideMode mode) override;
    bool present(dw3::Gpu& gpu, Capture* capture) override;
    void attach(dw3::Gpu& gpu) override;
    bool set_texture_dump(const std::string& directory) override;
    bool write_texture_dump(std::string& error) override;

private:
    HwPresenter();

    SDL_Window* window_ = nullptr;
    dw3::GraphicsApi api_ = dw3::GraphicsApi::Vulkan;
    dw3::NativeWindow native_;
    std::string name_;
    PresentOptions options_;
    std::unique_ptr<dw3::HwRenderer> renderer_;
};

}  // namespace dw3app
