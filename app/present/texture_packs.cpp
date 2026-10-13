#include "texture_packs.h"

#include <SDL3/SDL.h>

#include "dw3/platform.h"

namespace dw3app {

std::shared_ptr<const dw3::texrep::TexturePack> TexturePackCache::get(const Settings::Graphics& g) {
    if (!g.hd_backgrounds || g.texture_pack.empty()) return nullptr;
    if (g.texture_pack == name_) return pack_;
    name_ = g.texture_pack;
    pack_.reset();
    if (!dw3::texrep::valid_pack_name(name_)) {
        SDL_Log("texture pack: \"%s\" is not a valid folder name", name_.c_str());
        return nullptr;
    }
    const std::filesystem::path dir = dw3::platform::user_data_dir() / "texture-packs" / dw3::platform::path_from_utf8(name_);
    std::string error;
    pack_ = dw3::texrep::TexturePack::load(dir, error);
    if (!pack_) SDL_Log("texture pack: %s", error.c_str());
    else SDL_Log("texture pack \"%s\": %zu images at %dx", name_.c_str(), pack_->size(), pack_->scale());
    return pack_;
}

}  // namespace dw3app
