#include "disc_check.h"

#include <algorithm>
#include <cctype>

#include "psxdisc/disc.h"
#include "psxdisc/iso9660.h"
#include "sha256.h"

namespace dw3setup {

const ExpectedDisc& retail_usa() {
    static const ExpectedDisc expected{"SLUS-01436", "7bb79c083852a68faacd8d921438daed550dd8e3a51df559f40fa9c6a32de068"};
    return expected;
}

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

DiscCheck fail(DiscStatus status, std::string message, std::string serial = {}) {
    DiscCheck r;
    r.status = status;
    r.message = std::move(message);
    r.serial = std::move(serial);
    return r;
}

}  // namespace

std::string boot_file_from_system_cnf(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key = upper(std::string(line.substr(0, eq)));
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
        if (key != "BOOT") continue;
        std::string_view value = line.substr(eq + 1);
        std::size_t start = value.find_last_of("\\:/");
        if (start != std::string_view::npos) value = value.substr(start + 1);
        std::size_t semicolon = value.find(';');
        if (semicolon != std::string_view::npos) value = value.substr(0, semicolon);
        std::string name = upper(std::string(value));
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        return name;
    }
    return {};
}

std::string serial_from_system_cnf(std::string_view text) {
    std::string name = boot_file_from_system_cnf(text);
    if (name.empty()) return {};
    std::string serial;
    for (char c : name) {
        if (c == '_') serial += '-';
        else if (c != '.') serial += c;
    }
    return serial;
}

std::string_view region_of_serial(std::string_view serial) {
    if (serial.size() < 4) return {};
    const std::string prefix = upper(std::string(serial.substr(0, 4)));
    if (prefix == "SLUS" || prefix == "SCUS") return "USA";
    if (prefix == "SLES" || prefix == "SCES") return "Europe";
    if (prefix == "SLPS" || prefix == "SLPM" || prefix == "SCPS" || prefix == "SCPM" || prefix == "PAPX") return "Japan";
    return {};
}

DiscCheck check_disc(const std::filesystem::path& image, const ExpectedDisc& expected) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(image, ec)) return fail(DiscStatus::NotFound, "That file does not exist.");
    auto disc = psxdisc::Disc::open(image);
    if (!disc)
        return fail(DiscStatus::UnsupportedFormat,
                    "This is not a raw CD image. Choose the .cue (or the .bin it points to) of a MODE2/2352 dump; "
                    "cooked 2048-byte .iso files cannot be used.");
    auto root = psxdisc::read_root(*disc);
    if (!root) return fail(DiscStatus::NotPlayStation, "This image has no readable CD file system, so it is not a PlayStation game disc.");
    const auto records = psxdisc::parse_directory(disc->read_file(root->lba, root->size));
    auto find = [&](std::string_view name) -> const psxdisc::DirRecord* {
        for (const auto& r : records)
            if (!r.is_dir && upper(r.name) == name) return &r;
        return nullptr;
    };
    const psxdisc::DirRecord* cnf = find("SYSTEM.CNF");
    if (!cnf) return fail(DiscStatus::NotPlayStation, "This image has no SYSTEM.CNF, so it is not a PlayStation game disc.");
    const auto cnf_data = disc->read_file(cnf->lba, cnf->size);
    const std::string_view cnf_text(reinterpret_cast<const char*>(cnf_data.data()), cnf_data.size());
    const std::string serial = serial_from_system_cnf(cnf_text);
    if (serial.empty()) return fail(DiscStatus::NotPlayStation, "SYSTEM.CNF names no boot file, so this is not a PlayStation game disc.");

    if (serial != expected.serial) {
        const std::string_view region = region_of_serial(serial);
        const std::string wanted = "Digimon World 3 (USA), serial " + expected.serial;
        if (region != "USA" && !region.empty())
            return fail(DiscStatus::WrongRegion,
                        "This is a " + std::string(region) + " disc (" + serial + "). Only the USA release is supported: " + wanted + ".", serial);
        return fail(DiscStatus::WrongGame, "This disc has serial " + serial + ", which is not Digimon World 3. Choose " + wanted + ".", serial);
    }

    const psxdisc::DirRecord* exe = find(boot_file_from_system_cnf(cnf_text));
    if (!exe) return fail(DiscStatus::NotPlayStation, "The boot executable named in SYSTEM.CNF is missing from the image.", serial);
    const auto exe_data = disc->read_file(exe->lba, exe->size);
    if (exe_data.size() != exe->size) return fail(DiscStatus::NotPlayStation, "The image is truncated: the boot executable cannot be read.", serial);
    if (sha256_hex(exe_data) != expected.exe_sha256)
        return fail(DiscStatus::ModifiedExecutable,
                    "The serial matches, but the game executable differs from the retail USA release (a bad dump, a patched copy or another revision).",
                    serial);
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    ok.serial = serial;
    return ok;
}

}  // namespace dw3setup
