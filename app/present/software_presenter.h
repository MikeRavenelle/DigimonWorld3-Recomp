#pragma once

#include "presenter.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace dw3app {

// SDL renderer presentation. Used as the fallback when no GPU path is available.
class SoftwarePresenter final : public Presenter {
public:
    static std::unique_ptr<SoftwarePresenter> create(SDL_Window* window, const PresentOptions& options,
                                                     std::string& error);
    ~SoftwarePresenter() override;

    const char* name() const override { return "software (SDL renderer)"; }
    void set_vsync(bool on) override;
    bool present(dw3::Gpu& gpu, Capture* capture) override;

private:
    SoftwarePresenter() = default;

    PresentOptions options_;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    std::vector<uint32_t> pixels_;
};

}  // namespace dw3app
