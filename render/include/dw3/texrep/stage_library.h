#pragma once

// What the renderer needs to know about every field map of the game to draw its background whole (see
// field_backdrop.h): for each map the layout of its cells and the pieces of each cell's picture, and an index from the
// keys the renderer computes for an uploaded picture (content hash, palette hash) to the cells that use it. The
// pictures themselves are not kept; a picture is read from the game files when the renderer wants its pixels.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dw3/texrep/pack.h"
#include "dw3/texrep/stage_pack.h"

namespace dw3::texrep {

class StageLibrary {
public:
    struct Tile {
        int col = 0, row = 0;
        int width = 0, height = 0;  // picture size in texels
        uint64_t block_hash = 0, clut_hash = 0;
        std::vector<StagePiece> pieces;
    };
    struct Map {
        std::string name;
        int cols = 0, rows = 0;
        std::vector<Tile> tiles;
    };
    struct TileRef {
        uint32_t map = 0, tile = 0;
    };
    // Reads and parses the stage pack of a map by name; false when it cannot.
    using Loader = std::function<bool(const std::string& map, StagePack& out, std::string& error)>;

    explicit StageLibrary(Loader loader = nullptr) : loader_(std::move(loader)) {}
    ~StageLibrary();
    StageLibrary(const StageLibrary&) = delete;
    StageLibrary& operator=(const StageLibrary&) = delete;

    // Indexes the maps of a disc image or an extracted copy of the disc (see StageSource) on a background thread.
    // Returns null and sets `error` when the path holds no stage packs. ready() turns true when the index is complete.
    static std::shared_ptr<StageLibrary> open_async(const std::filesystem::path& path, std::string& error);

    // Adds one map to the index (for libraries that are filled by hand). Not safe while other threads read.
    void add_map(std::string name, const StagePack& pack);
    void finish() { ready_ = true; }

    bool ready() const { return ready_; }
    // Blocks until the background indexing is done (tests and tools that need the same result on every run).
    void wait_until_ready() {
        if (worker_.joinable()) worker_.join();
    }
    std::size_t map_count() const { return maps_.size(); }
    const Map& map(uint32_t index) const { return maps_[index]; }
    // The cells that hold a picture with these keys, over all maps.
    std::span<const TileRef> refs(uint64_t block_hash, uint64_t clut_hash) const;

    // The picture of one tile, RGBA8 with palette entry zero transparent. Reads the map's pack through the loader (the
    // most recent map is kept). Thread safe.
    bool tile_image(uint32_t map, uint32_t tile, Image& out);

private:
    struct KeyHash {
        std::size_t operator()(const std::pair<uint64_t, uint64_t>& k) const { return static_cast<std::size_t>(k.first * 0x9E3779B97F4A7C15ull ^ k.second); }
    };
    Loader loader_;
    std::vector<Map> maps_;
    std::unordered_map<std::pair<uint64_t, uint64_t>, std::vector<TileRef>, KeyHash> index_;
    std::atomic<bool> ready_{false};
    std::thread worker_;
    std::mutex pack_mutex_;
    int cached_map_ = -1;
    StagePack cached_pack_;
};

}  // namespace dw3::texrep
