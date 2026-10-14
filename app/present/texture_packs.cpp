#include "texture_packs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

#include "dw3/platform.h"

namespace dw3app {

namespace {

std::filesystem::path packs_dir() { return dw3::platform::user_data_dir() / "texture-packs"; }

// The first folder (in name order) under texture-packs that holds a manifest, or an empty string.
std::string find_any_pack() {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(packs_dir(), ec))
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "manifest.toml", ec))
            names.push_back(dw3::platform::path_to_utf8(entry.path().filename()));
    std::sort(names.begin(), names.end());
    return names.empty() ? std::string() : names.front();
}

}  // namespace

std::shared_ptr<const dw3::texrep::TexturePack> TexturePackCache::get(const Settings::Graphics& g) {
    if (!g.hd_backgrounds) return nullptr;
    std::string wanted = g.texture_pack;
    if (wanted.empty()) {
        // No pack named: use the first one that is installed.
        if (!searched_) {
            searched_ = true;
            discovered_ = find_any_pack();
            if (!discovered_.empty()) SDL_Log("texture pack: using \"%s\" (none selected in graphics.texture_pack)", discovered_.c_str());
        }
        wanted = discovered_;
    }
    if (wanted.empty()) return nullptr;
    if (wanted == name_) return pack_;
    name_ = wanted;
    pack_.reset();
    if (!dw3::texrep::valid_pack_name(name_)) {
        SDL_Log("texture pack: \"%s\" is not a valid folder name", name_.c_str());
        return nullptr;
    }
    std::string error;
    pack_ = dw3::texrep::TexturePack::load(packs_dir() / dw3::platform::path_from_utf8(name_), error);
    if (!pack_) SDL_Log("texture pack: %s", error.c_str());
    else SDL_Log("texture pack \"%s\": %zu images at %dx", name_.c_str(), pack_->size(), pack_->scale());
    return pack_;
}

std::shared_ptr<dw3::texrep::StageLibrary> StageLibraryCache::get(const Settings::Graphics& g, const std::string& disc_path) {
    if (!g.seamless_bg && g.widescreen == 0) return nullptr;
    if (disc_path != disc_ || !library_) {
        disc_ = disc_path;
        std::string error;
        library_ = dw3::texrep::StageLibrary::open_async(dw3::platform::path_from_utf8(disc_path), error);
        if (!library_) SDL_Log("seamless backgrounds: %s", error.c_str());
    }
    return library_;
}

}  // namespace dw3app
