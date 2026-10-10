#include "psxdisc/iso9660.h"

#include <set>

namespace psxdisc {
namespace {

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void walk_impl(Disc& disc, uint32_t lba, uint32_t size, const std::string& prefix, std::set<uint32_t>& seen,
               std::vector<FileEntry>& out) {
    if (!seen.insert(lba).second) return;  // guard against cycles
    auto data = disc.read_file(lba, (size + kForm1DataSize - 1) / kForm1DataSize * kForm1DataSize);
    for (const auto& rec : parse_directory(data)) {
        if (rec.is_dir) {
            walk_impl(disc, rec.lba, rec.size, prefix + rec.name + "/", seen, out);
        } else {
            out.push_back({prefix + rec.name, rec.lba, rec.size});
        }
    }
}

}  // namespace

std::vector<DirRecord> parse_directory(std::span<const uint8_t> data) {
    std::vector<DirRecord> out;
    std::size_t i = 0;
    while (i < data.size()) {
        uint8_t len = data[i];
        if (len == 0) {
            // Records never straddle sectors; zero length means skip to next sector.
            i = (i / kForm1DataSize + 1) * kForm1DataSize;
            continue;
        }
        if (len < 34 || i + len > data.size()) break;
        const uint8_t* r = data.data() + i;
        uint8_t name_len = r[32];
        if (33u + name_len > len) break;
        bool special = name_len == 1 && (r[33] == 0 || r[33] == 1);
        if (!special) {
            DirRecord rec;
            rec.lba = le32(r + 2);
            rec.size = le32(r + 10);
            rec.is_dir = (r[25] & 0x02) != 0;
            rec.name.assign(reinterpret_cast<const char*>(r + 33), name_len);
            if (auto semi = rec.name.find(';'); semi != std::string::npos) rec.name.resize(semi);
            out.push_back(std::move(rec));
        }
        i += len;
    }
    return out;
}

std::optional<DirRecord> read_root(Disc& disc) {
    uint8_t pvd[kForm1DataSize];
    if (!disc.read_form1(16, pvd)) return std::nullopt;
    if (pvd[0] != 1 || std::string(reinterpret_cast<char*>(pvd + 1), 5) != "CD001") return std::nullopt;
    const uint8_t* r = pvd + 156;
    return DirRecord{"", le32(r + 2), le32(r + 10), true};
}

std::vector<FileEntry> walk(Disc& disc, uint32_t dir_lba, uint32_t dir_size, const std::string& prefix) {
    std::vector<FileEntry> out;
    std::set<uint32_t> seen;
    walk_impl(disc, dir_lba, dir_size, prefix, seen, out);
    return out;
}

}  // namespace psxdisc
