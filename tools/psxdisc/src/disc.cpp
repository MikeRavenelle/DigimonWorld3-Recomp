#include "psxdisc/disc.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace psxdisc {

std::optional<CueSheet> parse_cue(const std::string& text) {
    CueSheet cue;
    std::istringstream in(text);
    std::string line;
    CueTrack* current = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return std::toupper(c); });
        if (cmd == "FILE") {
            if (!cue.bin_file.empty()) return std::nullopt;  // multi-file cues unsupported
            auto q1 = line.find('"');
            auto q2 = line.rfind('"');
            if (q1 == std::string::npos || q2 == q1) return std::nullopt;
            cue.bin_file = line.substr(q1 + 1, q2 - q1 - 1);
        } else if (cmd == "TRACK") {
            CueTrack t;
            ls >> t.number >> t.mode;
            cue.tracks.push_back(t);
            current = &cue.tracks.back();
        } else if (cmd == "INDEX" && current) {
            int idx = 0;
            std::string msf;
            ls >> idx >> msf;
            if (idx == 1) {
                int m = 0, s = 0, f = 0;
                if (std::sscanf(msf.c_str(), "%d:%d:%d", &m, &s, &f) != 3) return std::nullopt;
                current->index1_lba = static_cast<uint32_t>((m * 60 + s) * 75 + f);
            }
        }
    }
    if (cue.bin_file.empty() || cue.tracks.empty()) return std::nullopt;
    return cue;
}

uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) {
    return (static_cast<uint32_t>(from_bcd(m)) * 60 + from_bcd(s)) * 75 + from_bcd(f) - 150;
}

void lba_to_msf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f) {
    lba += 150;
    m = to_bcd(static_cast<uint8_t>(lba / (60 * 75)));
    s = to_bcd(static_cast<uint8_t>((lba / 75) % 60));
    f = to_bcd(static_cast<uint8_t>(lba % 75));
}

std::optional<Disc> Disc::open(const std::filesystem::path& path) {
    std::filesystem::path bin = path;
    if (path.extension() == ".cue" || path.extension() == ".CUE") {
        std::ifstream cf(path);
        if (!cf) return std::nullopt;
        std::stringstream ss;
        ss << cf.rdbuf();
        auto cue = parse_cue(ss.str());
        if (!cue || cue->tracks.front().mode != "MODE2/2352") return std::nullopt;
        bin = path.parent_path() / cue->bin_file;
    }
    std::error_code ec;
    auto size = std::filesystem::file_size(bin, ec);
    if (ec || size % kRawSectorSize != 0) return std::nullopt;
    std::ifstream f(bin, std::ios::binary);
    if (!f) return std::nullopt;
    return Disc(std::move(f), static_cast<uint32_t>(size / kRawSectorSize));
}

bool Disc::read_raw(uint32_t lba, std::span<uint8_t, kRawSectorSize> out) {
    if (lba >= sector_count_) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(lba) * kRawSectorSize);
    file_.read(reinterpret_cast<char*>(out.data()), kRawSectorSize);
    return static_cast<bool>(file_);
}

SectorHeader Disc::read_header(uint32_t lba) {
    uint8_t raw[kRawSectorSize]{};
    read_raw(lba, raw);
    return SectorHeader{raw[12], raw[13], raw[14], raw[15], raw[16], raw[17], raw[18], raw[19]};
}

bool Disc::read_form1(uint32_t lba, std::span<uint8_t, kForm1DataSize> out) {
    uint8_t raw[kRawSectorSize];
    if (!read_raw(lba, raw)) return false;
    std::copy_n(raw + 24, kForm1DataSize, out.begin());
    return true;
}

std::vector<uint8_t> Disc::read_file(uint32_t lba, uint32_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    uint8_t buf[kForm1DataSize];
    while (out.size() < size) {
        if (!read_form1(lba++, buf)) return {};
        auto n = std::min<std::size_t>(kForm1DataSize, size - out.size());
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

}  // namespace psxdisc
