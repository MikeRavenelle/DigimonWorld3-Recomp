#include "dw3/texrep/generate.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "dw3/texrep/blocks.h"
#include "dw3/texrep/dump.h"
#include "dw3/texrep/window.h"

namespace dw3::texrep {

namespace {

bool read_text(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

Image box_downscale(const Image& img, int factor) {
    if (factor <= 1) return img;
    Image out;
    out.w = img.w / factor;
    out.h = img.h / factor;
    out.rgba.resize(static_cast<std::size_t>(out.w) * out.h);
    const int n = factor * factor;
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int j = 0; j < factor; ++j)
                for (int i = 0; i < factor; ++i) {
                    const uint32_t p = img.rgba[static_cast<std::size_t>(y * factor + j) * img.w + x * factor + i];
                    for (int c = 0; c < 4; ++c) sum[c] += static_cast<int>((p >> (8 * c)) & 255);
                }
            uint32_t q = 0;
            for (int c = 0; c < 4; ++c) q |= static_cast<uint32_t>((sum[c] + n / 2) / n) << (8 * c);
            out.rgba[static_cast<std::size_t>(y) * out.w + x] = q;
        }
    return out;
}

bool generate_pack(const std::vector<std::filesystem::path>& dump_dirs, const std::filesystem::path& out_dir,
                   const GenerateOptions& options, const BatchUpscaler& upscale, GenerateReport& report, std::string& error) {
    if (options.scale < 1 || options.scale > kMaxPackScale || options.model_scale % options.scale != 0) {
        error = "the pack scale must divide the model scale (" + std::to_string(options.model_scale) + ") and be at most " +
                std::to_string(kMaxPackScale);
        return false;
    }
    PackManifest manifest;
    manifest.name = options.name;
    manifest.scale = options.scale;
    std::set<std::pair<uint64_t, uint64_t>> have;
    {
        std::string text;
        if (read_text(out_dir / "manifest.toml", text)) {
            PackManifest old;
            if (!parse_pack_manifest(text, old, error)) return false;
            if (old.scale != options.scale) {
                error = "the existing pack in " + out_dir.string() + " has scale " + std::to_string(old.scale);
                return false;
            }
            if (manifest.name.empty()) manifest.name = old.name;
            for (PackEntry& e : old.entries) have.insert({e.block, e.clut});
            manifest.entries = std::move(old.entries);
        }
    }

    // Collect the work: every dumped texture the pack does not have yet, with its component image.
    struct Item {
        DumpTexture texture;
        std::filesystem::path dir;
        std::size_t component;
    };
    std::vector<Item> items;
    std::vector<Image> components;
    for (const std::filesystem::path& dir : dump_dirs) {
        std::string text;
        if (!read_text(dir / "dump.toml", text)) {
            error = "no dump.toml in " + dir.string();
            return false;
        }
        DumpManifest dm;
        if (!parse_dump_manifest(text, dm, error)) return false;
        std::map<int, std::size_t> component_index;
        for (const DumpComponent& c : dm.components) {
            Image img;
            if (!read_png(dir / c.file, img, error)) return false;
            component_index[c.id] = components.size();
            components.push_back(std::move(img));
        }
        for (const DumpTexture& t : dm.textures) {
            if (have.count({t.block, t.clut})) {
                ++report.skipped_existing;
                continue;
            }
            have.insert({t.block, t.clut});
            items.push_back({t, dir, component_index[t.component]});
        }
    }

    const int shrink = options.model_scale / options.scale;
    for (std::size_t start = 0; start < items.size(); start += static_cast<std::size_t>(options.batch)) {
        const std::size_t end = std::min(items.size(), start + static_cast<std::size_t>(options.batch));
        std::vector<Image> windows;
        for (std::size_t i = start; i < end; ++i) {
            const Item& it = items[i];
            Image block;
            if (!read_png(it.dir / it.texture.file, block, error)) return false;
            windows.push_back(context_window(components[it.component], it.texture.x, it.texture.y, block, options.pad));
            bleed_transparent(windows.back(), options.pad);
        }
        std::vector<Image> up;
        if (!upscale(windows, up, error)) return false;
        if (up.size() != windows.size()) {
            error = "the upscaler returned the wrong number of images";
            return false;
        }
        for (std::size_t i = start; i < end; ++i) {
            const Item& it = items[i];
            const Image& big = up[i - start];
            const Image& src = windows[i - start];
            if (big.w != src.w * options.model_scale || big.h != src.h * options.model_scale) {
                error = "the upscaler returned an image of the wrong size";
                return false;
            }
            const Image scaled = box_downscale(big, shrink);
            const Image result = crop_block(scaled, options.pad, options.scale, it.texture.width, it.texture.height);
            PackEntry e;
            e.block = it.texture.block;
            e.clut = it.texture.clut;
            e.width = it.texture.width;
            e.height = it.texture.height;
            e.file = "tex/" + hash_to_hex(e.block) + "_" + hash_to_hex(e.clut) + ".png";
            if (!write_png(out_dir / e.file, result, error, false)) return false;
            manifest.entries.push_back(std::move(e));
            ++report.generated;
        }
        if (options.progress) options.progress(end, items.size());
    }

    const std::string text = write_pack_manifest(manifest);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::ofstream out(out_dir / "manifest.toml", std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "cannot write " + (out_dir / "manifest.toml").string();
        return false;
    }
    return true;
}

}  // namespace dw3::texrep
