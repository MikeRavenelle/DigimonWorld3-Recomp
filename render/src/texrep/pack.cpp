#include "dw3/texrep/pack.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "dw3/texrep/blocks.h"

#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
#include "stb_image.h"
#include "stb_image_write.h"

namespace dw3::texrep {

namespace {

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

void append_to_stream(void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

}  // namespace

bool decode_png(const uint8_t* data, std::size_t size, Image& out, std::string& error) {
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!px) {
        error = std::string("cannot decode PNG: ") + stbi_failure_reason();
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h);
    std::memcpy(out.rgba.data(), px, out.rgba.size() * 4);
    stbi_image_free(px);
    return true;
}

bool read_png(const std::filesystem::path& path, Image& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        error = "cannot read " + path.string();
        return false;
    }
    if (!decode_png(bytes.data(), bytes.size(), out, error)) {
        error += " (" + path.string() + ")";
        return false;
    }
    return true;
}

bool write_png(const std::filesystem::path& path, const Image& img, std::string& error, bool with_alpha) {
    std::string bytes;
    std::vector<uint8_t> rgb;
    if (!with_alpha) {
        rgb.reserve(img.rgba.size() * 3);
        for (uint32_t p : img.rgba) {
            rgb.push_back(static_cast<uint8_t>(p));
            rgb.push_back(static_cast<uint8_t>(p >> 8));
            rgb.push_back(static_cast<uint8_t>(p >> 16));
        }
    }
    const int channels = with_alpha ? 4 : 3;
    const void* data = with_alpha ? static_cast<const void*>(img.rgba.data()) : static_cast<const void*>(rgb.data());
    if (!stbi_write_png_to_func(append_to_stream, &bytes, img.w, img.h, channels, data, img.w * channels)) {
        error = "cannot encode PNG";
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool parse_pack_manifest(std::string_view text, PackManifest& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(std::string(text), std::string_view("manifest.toml"));
    } catch (const toml::parse_error& e) {
        std::ostringstream ss;
        ss << e;
        error = ss.str();
        return false;
    }
    PackManifest m;
    const int format = static_cast<int>(root["format"].value_or<int64_t>(0));
    if (format != kPackFormat) {
        error = "unsupported pack format " + std::to_string(format);
        return false;
    }
    m.name = root["name"].value_or<std::string>("");
    m.scale = static_cast<int>(root["scale"].value_or<int64_t>(0));
    if (m.scale < 1 || m.scale > kMaxPackScale) {
        error = "scale must be 1 to " + std::to_string(kMaxPackScale);
        return false;
    }
    if (const toml::array* arr = root["texture"].as_array()) {
        for (const toml::node& n : *arr) {
            const toml::table* t = n.as_table();
            if (!t) continue;
            PackEntry e;
            const std::string block = (*t)["block"].value_or<std::string>("");
            const std::string clut = (*t)["clut"].value_or<std::string>("");
            if (!hash_from_hex(block, e.block) || !hash_from_hex(clut, e.clut)) {
                error = "texture entry with a bad block or clut hash";
                return false;
            }
            e.width = static_cast<int>((*t)["width"].value_or<int64_t>(0));
            e.height = static_cast<int>((*t)["height"].value_or<int64_t>(0));
            e.file = (*t)["file"].value_or<std::string>("");
            if (e.width < 1 || e.height < 1 || e.width > 512 || e.height > 512 || e.file.empty()) {
                error = "texture entry " + block + " has a bad size or no file";
                return false;
            }
            if (e.file.find("..") != std::string::npos || e.file[0] == '/' || e.file[0] == '\\' ||
                e.file.find(':') != std::string::npos) {
                error = "texture file name must stay inside the pack: " + e.file;
                return false;
            }
            m.entries.push_back(std::move(e));
        }
    }
    out = std::move(m);
    return true;
}

std::string write_pack_manifest(const PackManifest& m) {
    toml::table root;
    root.insert("format", static_cast<int64_t>(kPackFormat));
    root.insert("name", m.name);
    root.insert("scale", static_cast<int64_t>(m.scale));
    toml::array arr;
    for (const PackEntry& e : m.entries) {
        toml::table t;
        t.insert("block", hash_to_hex(e.block));
        t.insert("clut", hash_to_hex(e.clut));
        t.insert("width", static_cast<int64_t>(e.width));
        t.insert("height", static_cast<int64_t>(e.height));
        t.insert("file", e.file);
        arr.push_back(std::move(t));
    }
    root.insert("texture", std::move(arr));
    std::ostringstream ss;
    ss << root;
    return ss.str();
}

std::shared_ptr<TexturePack> TexturePack::from_manifest(PackManifest m, std::filesystem::path dir) {
    auto p = std::shared_ptr<TexturePack>(new TexturePack());
    p->manifest_ = std::move(m);
    p->dir_ = std::move(dir);
    for (std::size_t i = 0; i < p->manifest_.entries.size(); ++i) p->blocks_[p->manifest_.entries[i].block].push_back(i);
    return p;
}

std::shared_ptr<TexturePack> TexturePack::load(const std::filesystem::path& dir, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_file(dir / "manifest.toml", bytes)) {
        error = "no manifest.toml in " + dir.string();
        return nullptr;
    }
    PackManifest m;
    if (!parse_pack_manifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), m, error)) return nullptr;
    return from_manifest(std::move(m), dir);
}

const PackEntry* TexturePack::find(uint64_t block, uint64_t clut) const {
    const auto it = blocks_.find(block);
    if (it == blocks_.end()) return nullptr;
    for (std::size_t i : it->second)
        if (manifest_.entries[i].clut == clut) return &manifest_.entries[i];
    return nullptr;
}

bool TexturePack::load_image(const PackEntry& e, Image& out, std::string& error) const {
    if (!read_png(dir_ / std::filesystem::path(e.file), out, error)) return false;
    if (out.w != e.width * manifest_.scale || out.h != e.height * manifest_.scale) {
        error = e.file + " is " + std::to_string(out.w) + "x" + std::to_string(out.h) + ", expected " +
                std::to_string(e.width * manifest_.scale) + "x" + std::to_string(e.height * manifest_.scale);
        return false;
    }
    return true;
}

bool valid_pack_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (char c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    return true;
}

}  // namespace dw3::texrep
