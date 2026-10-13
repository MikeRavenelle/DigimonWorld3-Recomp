#pragma once

// Texture pack generation from texture dumps (see dump.h). The upscaler itself is a callback, so this part is
// independent of any particular program and can be tested.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dw3/texrep/pack.h"

namespace dw3::texrep {

// Upscales every image of `in` by the model's factor (the results have exactly that many times the width and height),
// in the same order.
using BatchUpscaler = std::function<bool(const std::vector<Image>& in, std::vector<Image>& out, std::string& error)>;

struct GenerateOptions {
    std::string name;       // pack name stored in the manifest
    int scale = 4;          // pixels per original texel in the pack; divides model_scale
    int model_scale = 4;    // factor of the upscaler
    int pad = 16;           // pixels of neighbouring context given to the upscaler around each block
    int batch = 24;         // images handed to the upscaler at once
    // Called after each batch with (done, total).
    std::function<void(std::size_t, std::size_t)> progress;
};

struct GenerateReport {
    std::size_t generated = 0, skipped_existing = 0;
};

// Reduces an image by an integer factor, averaging each factor x factor square.
Image box_downscale(const Image& img, int factor);

// Builds the pack in `out_dir` from the dumps in `dump_dirs`. Images the pack already holds (same block and palette) are
// kept and not generated again, so packs can be extended with further dumps. Returns false and sets `error` on failure.
bool generate_pack(const std::vector<std::filesystem::path>& dump_dirs, const std::filesystem::path& out_dir,
                   const GenerateOptions& options, const BatchUpscaler& upscale, GenerateReport& report, std::string& error);

}  // namespace dw3::texrep
