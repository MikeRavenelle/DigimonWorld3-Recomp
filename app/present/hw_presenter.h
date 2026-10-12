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
    void set_vsync(bool on) override;
    bool present(dw3::Gpu& gpu, Capture* capture) override;
    void attach(dw3::Gpu& gpu) override;

private:
    HwPresenter();

    SDL_Window* window_ = nullptr;
    dw3::NativeWindow native_;
    std::string name_;
    PresentOptions options_;
    std::unique_ptr<dw3::HwRenderer> renderer_;
};

}  // namespace dw3app
