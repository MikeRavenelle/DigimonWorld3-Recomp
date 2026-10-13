#include "zip_extract.h"

#include <miniz.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "dw3/platform.h"

namespace dw3setup {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> split_path(const std::string& name) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : name) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}

}  // namespace

bool extract_zip(const fs::path& zip, const fs::path& destination, std::string& error) {
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, dw3::platform::path_to_utf8(zip).c_str(), 0)) {
        error = "cannot open the archive";
        return false;
    }
    struct Closer {
        mz_zip_archive* a;
        ~Closer() { mz_zip_reader_end(a); }
    } closer{&archive};

    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    std::vector<std::vector<std::string>> entries(count);
    bool single_root = count > 0;
    std::string root;
    for (mz_uint i = 0; i < count; ++i) {
        char name[1024];
        if (!mz_zip_reader_get_filename(&archive, i, name, sizeof name)) {
            error = "unreadable archive entry";
            return false;
        }
        entries[i] = split_path(name);
        for (const std::string& part : entries[i])
            if (part == "..") {
                error = std::string("unsafe path in archive: ") + name;
                return false;
            }
        if (entries[i].empty()) continue;
        const bool is_dir = mz_zip_reader_is_file_a_directory(&archive, i);
        if (entries[i].size() == 1 && !is_dir) single_root = false;  // a file at the top level
        if (root.empty()) root = entries[i][0];
        else if (root != entries[i][0]) single_root = false;
    }

    std::error_code ec;
    fs::create_directories(destination, ec);
    for (mz_uint i = 0; i < count; ++i) {
        std::vector<std::string>& parts = entries[i];
        if (single_root && !parts.empty()) parts.erase(parts.begin());
        if (parts.empty() || mz_zip_reader_is_file_a_directory(&archive, i)) continue;
        fs::path target = destination;
        for (const std::string& part : parts) target /= dw3::platform::path_from_utf8(part);
        fs::create_directories(target.parent_path(), ec);

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            error = "unreadable archive entry";
            return false;
        }
        std::vector<char> data(static_cast<std::size_t>(stat.m_uncomp_size));
        if (!data.empty() && !mz_zip_reader_extract_to_mem(&archive, i, data.data(), data.size(), 0)) {
            error = "cannot unpack " + std::string(stat.m_filename) + " (damaged archive)";
            return false;
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) {
            error = "cannot write " + dw3::platform::path_to_utf8(target);
            return false;
        }
        const unsigned unix_mode = (stat.m_external_attr >> 16) & 0xFFFF;
        if ((unix_mode & 0100) != 0)
            fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    }
    return true;
}

}  // namespace dw3setup
