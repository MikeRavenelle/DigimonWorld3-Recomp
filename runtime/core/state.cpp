#include "dw3/state.h"

#include "dw3/platform.h"

#include <zstd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace dw3 {

StateIo::StateIo(std::span<const uint8_t> data) : loading_(true), in_(data), limit_(data.size()) {}

void StateIo::bytes(void* p, std::size_t n) {
    if (!loading_) {
        const auto* b = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
        return;
    }
    if (n > limit() - pos_) throw StateError("save state is truncated");
    if (n) std::memcpy(p, in_.data() + pos_, n);
    pos_ += n;
}

std::size_t StateIo::length(std::size_t n) {
    if (!loading_ && n > kMaxElements) throw StateError("container too large to save");
    uint32_t v = static_cast<uint32_t>(n);
    io(v);
    if (v > kMaxElements) throw StateError("save state has an implausible length");
    return v;
}

void StateIo::io(std::string& s) {
    std::size_t n = length(s.size());
    if (loading_) s.resize(n);
    bytes(s.data(), n);
}

void StateIo::begin(const char (&tag)[5], uint32_t version, uint32_t min_version) {
    Frame f{};
    std::memcpy(f.tag, tag, 5);
    f.version = version;
    if (!loading_) {
        out_.insert(out_.end(), tag, tag + 4);
        f.start = out_.size();
        uint32_t v = version, zero = 0;
        io(v);
        io(zero);
        frames_.push_back(f);
        return;
    }
    for (;;) {  // skip sections this build does not know
        if (limit() - pos_ < 12) throw StateError(std::string("save state has no ") + tag + " section");
        char got[5] = {};
        bytes(got, 4);
        uint32_t ver = 0, len = 0;
        io(ver);
        io(len);
        if (len > limit() - pos_) throw StateError(std::string("save state section ") + got + " is truncated");
        if (std::memcmp(got, tag, 4) != 0) {
            pos_ += len;
            continue;
        }
        if (ver > version)
            throw StateError(std::string("save state section ") + tag + " has version " + std::to_string(ver) +
                             ", this build reads up to " + std::to_string(version) + " (state from a newer build)");
        if (ver < min_version)
            throw StateError(std::string("save state section ") + tag + " has version " + std::to_string(ver) +
                             ", older than the oldest supported (" + std::to_string(min_version) + ")");
        f.version = ver;
        f.end = pos_ + len;
        frames_.push_back(f);
        return;
    }
}

void StateIo::end() {
    if (frames_.empty()) throw StateError("unbalanced save state section");
    Frame f = frames_.back();
    frames_.pop_back();
    if (!loading_) {
        uint32_t len = static_cast<uint32_t>(out_.size() - (f.start + 8));
        for (std::size_t i = 0; i < 4; ++i) out_[f.start + 4 + i] = static_cast<uint8_t>(len >> (8 * i));
    } else if (pos_ != f.end) {
        throw StateError(std::string("save state section ") + f.tag + " does not match its layout version");
    }
}

namespace savestate {

namespace {
constexpr char kMagic[4] = {'D', 'W', '3', 'S'};
constexpr std::size_t kHeaderSize = 4 + 4 + 4 + 8 + 8 + 8 + 4;
constexpr int kLevel = 6;

uint32_t crc32(std::span<const uint8_t> d) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return ~c;
}
}  // namespace

std::vector<uint8_t> wrap(std::span<const uint8_t> stored, uint64_t identity, const Info& info) {
    StateIo h;
    char magic[4] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3]};
    h.bytes(magic, 4);
    uint32_t version = kContainerVersion, encoding = info.encoding, crc = crc32(stored);
    uint64_t size = stored.size(), decoded = info.decoded_size;
    h.io(version);
    h.io(encoding);
    h.io(identity);
    h.io(size);
    h.io(decoded);
    h.io(crc);
    std::vector<uint8_t> out = h.take();
    std::size_t bound = ZSTD_compressBound(stored.size());
    out.resize(kHeaderSize + bound);
    std::size_t n = ZSTD_compress(out.data() + kHeaderSize, bound, stored.data(), stored.size(), kLevel);
    if (ZSTD_isError(n)) throw StateError(std::string("cannot compress save state: ") + ZSTD_getErrorName(n));
    out.resize(kHeaderSize + n);
    return out;
}

std::vector<uint8_t> unwrap(std::span<const uint8_t> file, uint64_t identity, Info& info) {
    if (file.size() < kHeaderSize || std::memcmp(file.data(), kMagic, 4) != 0)
        throw StateError("not a Digimon World 3 save state");
    StateIo h(file.first(kHeaderSize));
    char magic[4];
    h.bytes(magic, 4);
    uint32_t version = 0, encoding = 0, crc = 0;
    uint64_t id = 0, size = 0, decoded = 0;
    h.io(version);
    h.io(encoding);
    h.io(id);
    h.io(size);
    h.io(decoded);
    h.io(crc);
    if (version > kContainerVersion)
        throw StateError("save state container has format version " + std::to_string(version) +
                         ", this build reads up to " + std::to_string(kContainerVersion));
    if (version < 1) throw StateError("save state has an invalid format version");
    if (encoding > kDiscRefs) throw StateError("save state uses an encoding this build does not know");
    if (id != identity) throw StateError("save state was written for a different game or disc");
    if (size > (std::size_t{1} << 30) || decoded > (std::size_t{1} << 30)) throw StateError("save state is implausibly large");
    std::vector<uint8_t> stored(size);
    std::size_t n = ZSTD_decompress(stored.data(), stored.size(), file.data() + kHeaderSize, file.size() - kHeaderSize);
    if (ZSTD_isError(n) || n != size) throw StateError("save state is damaged (cannot decompress)");
    if (crc32(stored) != crc) throw StateError("save state is damaged (checksum mismatch)");
    info.encoding = encoding;
    info.decoded_size = decoded;
    return stored;
}

bool write_file(const std::string& path, std::span<const uint8_t> file, std::string& error) {
    std::error_code ec;
    const std::filesystem::path target = platform::path_from_utf8(path);
    auto parent = target.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    // Written beside the target and renamed over it, so a reader never sees a half-written file.
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (f) f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (f) f.flush();
        if (!f) {
            error = "cannot write " + path;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        error = "cannot write " + path + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& file, std::string& error) {
    std::ifstream f(platform::path_from_utf8(path), std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace savestate

}  // namespace dw3
