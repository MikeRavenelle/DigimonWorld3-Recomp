#include "dw3/memcard.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace dw3::memcard {

namespace {

constexpr std::size_t kEntryNameOffset = 0x0A;
constexpr std::size_t kGmeHeader = 3904, kVgsHeader = 64;

uint32_t rd32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}
void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

bool is_free_state(uint8_t s) { return s >= kStateFree && s <= kStateDeletedLast; }

void write_free_entry(std::span<uint8_t> f) {
    std::fill(f.begin(), f.end(), uint8_t{0});
    f[0] = kStateFree;
    wr16(f.data() + 8, kNoNext);
    seal_frame(f);
}

std::string entry_name(const uint8_t* f) {
    const char* n = reinterpret_cast<const char*>(f + kEntryNameOffset);
    return std::string(n, ::strnlen(n, kMaxNameLen));
}

bool valid_name(std::string_view n) { return !n.empty() && n.size() <= kMaxNameLen && n.find('\0') == n.npos; }

}  // namespace

uint8_t frame_checksum(std::span<const uint8_t> f) {
    uint8_t c = 0;
    for (std::size_t i = 0; i < kFrameSize - 1 && i < f.size(); ++i) c ^= f[i];
    return c;
}

void seal_frame(std::span<uint8_t> f) { f[kFrameSize - 1] = frame_checksum(f); }

bool frame_checksum_ok(std::span<const uint8_t> f) {
    return f.size() >= kFrameSize && f[kFrameSize - 1] == frame_checksum(f);
}

void format_memory_card(std::span<uint8_t, kCardSize> image) {
    std::fill(image.begin(), image.end(), uint8_t{0});
    auto frame = [&](std::size_t n) { return std::span<uint8_t>(image.data() + n * kFrameSize, kFrameSize); };
    auto f0 = frame(0);
    f0[0] = 'M';
    f0[1] = 'C';
    seal_frame(f0);
    for (std::size_t n = 1; n <= 15; ++n) write_free_entry(frame(n));
    for (std::size_t n = 16; n <= 35; ++n) {  // broken sector list: none
        auto f = frame(n);
        std::fill_n(f.begin(), 4, uint8_t{0xFF});
        seal_frame(f);
    }
    std::memcpy(frame(63).data(), f0.data(), kFrameSize);  // write-test frame
}

bool is_formatted(std::span<const uint8_t> image) {
    if (image.size() != kCardSize || image[0] != 'M' || image[1] != 'C') return false;
    for (std::size_t n = 0; n <= 15; ++n)
        if (!frame_checksum_ok(image.subspan(n * kFrameSize, kFrameSize))) return false;
    return true;
}

std::vector<FileInfo> CardFs::list() const {
    std::vector<FileInfo> out;
    for (int b = 1; b <= kDataBlocks; ++b) {
        const uint8_t* e = dir_frame(b);
        if (e[0] != kStateFirst) continue;
        FileInfo fi;
        fi.name = entry_name(e);
        fi.size = rd32(e + 4);
        // Follow the chain; guard against loops in corrupt images.
        int cur = b;
        for (int guard = 0; guard < kDataBlocks; ++guard) {
            fi.blocks.push_back(static_cast<uint8_t>(cur));
            uint16_t next = rd16(dir_frame(cur) + 8);
            if (next == kNoNext || next >= kDataBlocks) break;
            cur = next + 1;
        }
        out.push_back(std::move(fi));
    }
    return out;
}

std::optional<FileInfo> CardFs::find(std::string_view name) const {
    for (auto& f : list())
        if (f.name == name) return f;
    return std::nullopt;
}

int CardFs::free_blocks() const {
    int n = 0;
    for (int b = 1; b <= kDataBlocks; ++b)
        if (is_free_state(dir_frame(b)[0])) ++n;
    return n;
}

Status CardFs::create(std::string_view name, int blocks) {
    if (!valid()) return Status::BadDevice;
    if (!valid_name(name) || blocks < 1 || blocks > kDataBlocks) return Status::BadArg;
    if (find(name)) return Status::Exists;
    std::vector<int> pick;
    for (int b = 1; b <= kDataBlocks && static_cast<int>(pick.size()) < blocks; ++b)
        if (is_free_state(dir_frame(b)[0])) pick.push_back(b);
    if (static_cast<int>(pick.size()) < blocks) return Status::NoSpace;
    for (std::size_t i = 0; i < pick.size(); ++i) {
        auto f = frame_span(pick[i]);
        std::fill(f.begin(), f.end(), uint8_t{0});
        bool first = i == 0, last = i + 1 == pick.size();
        f[0] = first ? kStateFirst : (last ? kStateLast : kStateMiddle);
        if (first) {
            wr32(f.data() + 4, static_cast<uint32_t>(blocks) * kBlockSize);
            std::memcpy(f.data() + kEntryNameOffset, name.data(), name.size());
        }
        wr16(f.data() + 8, last ? kNoNext : static_cast<uint16_t>(pick[i + 1] - 1));
        seal_frame(f);
        // Fresh data blocks start out zeroed.
        std::memset(img_.data() + static_cast<std::size_t>(pick[i]) * kBlockSize, 0, kBlockSize);
    }
    return Status::Ok;
}

std::size_t CardFs::read(const FileInfo& f, std::size_t offset, std::span<uint8_t> out) const {
    std::size_t total = std::min<std::size_t>(f.size, f.blocks.size() * kBlockSize);
    if (offset >= total) return 0;
    std::size_t n = std::min(out.size(), total - offset), done = 0;
    while (done < n) {
        std::size_t pos = offset + done, blk = f.blocks[pos / kBlockSize], in = pos % kBlockSize;
        std::size_t chunk = std::min(n - done, kBlockSize - in);
        std::memcpy(out.data() + done, img_.data() + blk * kBlockSize + in, chunk);
        done += chunk;
    }
    return n;
}

std::size_t CardFs::write(const FileInfo& f, std::size_t offset, std::span<const uint8_t> in) {
    std::size_t total = std::min<std::size_t>(f.size, f.blocks.size() * kBlockSize);
    if (offset >= total) return 0;
    std::size_t n = std::min(in.size(), total - offset), done = 0;
    while (done < n) {
        std::size_t pos = offset + done, blk = f.blocks[pos / kBlockSize], at = pos % kBlockSize;
        std::size_t chunk = std::min(n - done, kBlockSize - at);
        std::memcpy(img_.data() + blk * kBlockSize + at, in.data() + done, chunk);
        done += chunk;
    }
    return n;
}

Status CardFs::remove(std::string_view name) {
    if (!valid()) return Status::BadDevice;
    auto f = find(name);
    if (!f) return Status::NotFound;
    for (uint8_t b : f->blocks) {
        auto e = frame_span(b);
        e[0] = e[0] == kStateFirst ? kStateDeletedFirst : e[0] == kStateMiddle ? kStateDeletedMiddle : kStateDeletedLast;
        seal_frame(e);
    }
    return Status::Ok;
}

Status CardFs::rename(std::string_view from, std::string_view to) {
    if (!valid()) return Status::BadDevice;
    if (!valid_name(to)) return Status::BadArg;
    auto f = find(from);
    if (!f) return Status::NotFound;
    if (from != to && find(to)) return Status::Exists;
    auto e = frame_span(f->blocks.front());
    std::fill_n(e.begin() + kEntryNameOffset, kMaxNameLen, uint8_t{0});
    std::memcpy(e.data() + kEntryNameOffset, to.data(), to.size());
    seal_frame(e);
    return Status::Ok;
}

std::optional<std::vector<uint8_t>> load_card_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t skip = 0;
    if (data.size() == kCardSize + kGmeHeader && std::memcmp(data.data(), "123-456-STD", 11) == 0) skip = kGmeHeader;
    else if (data.size() == kCardSize + kVgsHeader && std::memcmp(data.data(), "VgsM", 4) == 0) skip = kVgsHeader;
    else if (data.size() != kCardSize) return std::nullopt;
    return std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(skip), data.end());
}

bool save_card_file(const std::filesystem::path& path, std::span<const uint8_t> image) {
    std::error_code ec;
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace dw3::memcard
