#include "dw3/texrep/stage_dump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

#include "dw3/texrep/blocks.h"
#include "psxdisc/iso9660.h"

namespace dw3::texrep {

namespace {

constexpr uint32_t kHiddenRootLba = 473;  // the game's own files are in a directory tree that the volume root does not link

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "S226PACK.BIN" -> "S226"
std::string map_of_file(const std::string& file_name) {
    const std::string u = upper(file_name);
    const std::string tail = "PACK.BIN";
    if (u.size() <= tail.size() + 1 || u[0] != 'S' || u.compare(u.size() - tail.size(), tail.size(), tail) != 0) return {};
    const std::string digits = u.substr(1, u.size() - tail.size() - 1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return {};
    return u.substr(0, u.size() - tail.size());
}

}  // namespace

std::unique_ptr<StageSource> StageSource::open(const std::filesystem::path& path, std::string& error) {
    auto source = std::unique_ptr<StageSource>(new StageSource());
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::filesystem::path stage_dir;
        for (const char* rel : {"AAA/DAT/Z_STAGE", "DAT/Z_STAGE", "Z_STAGE", "."}) {
            const std::filesystem::path candidate = path / rel;
            if (!std::filesystem::is_directory(candidate, ec)) continue;
            for (const auto& entry : std::filesystem::directory_iterator(candidate, ec))
                if (entry.is_regular_file() && !map_of_file(entry.path().filename().string()).empty()) {
                    stage_dir = candidate;
                    break;
                }
            if (!stage_dir.empty()) break;
        }
        if (stage_dir.empty()) {
            error = "no stage pack files (S<nnn>PACK.BIN) found in " + path.string();
            return nullptr;
        }
        for (const auto& entry : std::filesystem::directory_iterator(stage_dir, ec)) {
            const std::string map = entry.is_regular_file() ? map_of_file(entry.path().filename().string()) : std::string();
            if (!map.empty()) source->dir_files_[map] = entry.path();
        }
        for (const auto& [name, file] : source->dir_files_) source->names_.push_back(name);
        return source;
    }
    source->disc_ = psxdisc::Disc::open(path);
    if (!source->disc_) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    const auto files = psxdisc::walk(*source->disc_, kHiddenRootLba, psxdisc::kForm1DataSize, "AAA/");
    for (const psxdisc::FileEntry& f : files) {
        const std::size_t slash = f.path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : upper(f.path.substr(0, slash));
        if (dir != "AAA/DAT/Z_STAGE") continue;
        const std::string map = map_of_file(slash == std::string::npos ? f.path : f.path.substr(slash + 1));
        if (!map.empty()) source->disc_files_[map] = {f.lba, f.size};
    }
    if (source->disc_files_.empty()) {
        error = "the disc image has no stage pack files; is it Digimon World 3 (USA)?";
        return nullptr;
    }
    for (const auto& [name, file] : source->disc_files_) source->names_.push_back(name);
    return source;
}

bool StageSource::read(const std::string& map, std::vector<uint8_t>& out, std::string& error) {
    if (disc_) {
        const auto it = disc_files_.find(map);
        if (it == disc_files_.end()) {
            error = "no stage pack for " + map;
            return false;
        }
        out = disc_->read_file(it->second.lba, it->second.size);
        if (out.size() != it->second.size) {
            error = "short read of the stage pack of " + map;
            return false;
        }
        return true;
    }
    const auto it = dir_files_.find(map);
    if (it == dir_files_.end()) {
        error = "no stage pack for " + map;
        return false;
    }
    std::ifstream in(it->second, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.empty()) {
        error = "cannot read " + it->second.string();
        return false;
    }
    return true;
}

bool StageDumpWriter::add_map(const std::string& name, const StagePack& pack, std::string& error) {
    ++stats_.maps;
    DumpComponent component;
    component.id = static_cast<int>(manifest_.components.size());
    component.file = "components/" + name + ".png";
    const Image map = compose_stage_map(pack);
    component.width = map.w;
    component.height = map.h;
    if (!write_png(dir_ / component.file, map, error)) return false;
    manifest_.components.push_back(component);

    for (const StageTile& tile : pack.tiles) {
        ++stats_.cells;
        if (!tile.tracked()) {
            ++stats_.untracked;
            continue;
        }
        if (!seen_.emplace(std::make_pair(tile.block_hash, tile.clut_hash), true).second) continue;
        const Image picture = decode_stage_tile(tile);
        if (std::none_of(picture.rgba.begin(), picture.rgba.end(), [](uint32_t c) { return (c >> 24) != 0; })) {
            ++stats_.transparent;
            continue;
        }
        DumpTexture t;
        t.block = tile.block_hash;
        t.clut = tile.clut_hash;
        t.width = picture.w;
        t.height = picture.h;
        t.file = "textures/" + hash_to_hex(t.block) + "_" + hash_to_hex(t.clut) + ".png";
        t.uses = 1;
        t.component = component.id;
        t.x = tile.col * kStageCell;
        t.y = tile.row * kStageCell;
        t.primary = true;
        for (const StagePiece& p : tile.pieces)
            t.pieces.push_back({p.ax, p.ay, p.w, p.h, tile.col * kStageCell + p.x, tile.row * kStageCell + p.y});
        if (!write_png(dir_ / t.file, picture, error)) return false;
        manifest_.textures.push_back(std::move(t));
        ++stats_.images;
    }
    return true;
}

bool StageDumpWriter::finish(std::string& error) {
    const std::string text = write_dump_manifest(manifest_);
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    std::ofstream out(dir_ / "dump.toml", std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "cannot write " + (dir_ / "dump.toml").string();
        return false;
    }
    return true;
}

}  // namespace dw3::texrep
