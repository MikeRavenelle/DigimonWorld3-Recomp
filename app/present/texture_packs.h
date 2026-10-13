// Finds and loads the texture pack that the graphics settings select.
#pragma once

#include <memory>
#include <string>

#include "dw3/texrep/pack.h"
#include "settings.h"

namespace dw3app {

class TexturePackCache {
public:
    // The pack named by graphics.texture_pack under <user data>/texture-packs, or, when none is named, the first one
    // installed there. Null when HD backgrounds are off, there is no pack or it cannot be loaded (the reason is logged
    // once per name). The loaded manifest is kept, so asking again for the same name is cheap.
    std::shared_ptr<const dw3::texrep::TexturePack> get(const Settings::Graphics& g);

private:
    std::string name_, discovered_;
    bool searched_ = false;
    std::shared_ptr<const dw3::texrep::TexturePack> pack_;
};

}  // namespace dw3app
