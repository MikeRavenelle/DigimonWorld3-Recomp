// dw3extract: lists / extracts the Digimon World 3 (USA) disc contents.
//
// The visible ISO9660 tree only contains SYSTEM.CNF, SLUS_014.36 and DUMMY. The real game
// data lives in an "AAA" directory tree whose root record sits at LBA 473 but is not linked
// from the volume root, so we walk it explicitly.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "psxdisc/disc.h"
#include "psxdisc/iso9660.h"

namespace fs = std::filesystem;

constexpr uint32_t kHiddenRootLba = 473;

static int usage() {
    std::fprintf(stderr,
                 "usage: dw3extract <disc.cue> list\n"
                 "       dw3extract <disc.cue> extract <out_dir>\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    auto disc = psxdisc::Disc::open(argv[1]);
    if (!disc) {
        std::fprintf(stderr, "failed to open disc: %s\n", argv[1]);
        return 1;
    }
    auto root = psxdisc::read_root(*disc);
    if (!root) {
        std::fprintf(stderr, "no ISO9660 primary volume descriptor\n");
        return 1;
    }
    auto files = psxdisc::walk(*disc, root->lba, root->size);
    auto hidden = psxdisc::walk(*disc, kHiddenRootLba, psxdisc::kForm1DataSize, "AAA/");
    files.insert(files.end(), hidden.begin(), hidden.end());

    std::string cmd = argv[2];
    if (cmd == "list") {
        for (const auto& f : files) std::printf("%s\t%u\t%u\n", f.path.c_str(), f.lba, f.size);
        return 0;
    }
    if (cmd == "extract" && argc >= 4) {
        fs::path out = argv[3];
        for (const auto& f : files) {
            if (f.path == "DUMMY") continue;  // 35MB of padding
            auto data = disc->read_file(f.lba, f.size);
            if (data.size() != f.size) {
                std::fprintf(stderr, "short read: %s\n", f.path.c_str());
                return 1;
            }
            fs::path dst = out / f.path;
            fs::create_directories(dst.parent_path());
            std::ofstream(dst, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
        }
        std::printf("extracted %zu files to %s\n", files.size(), out.string().c_str());
        return 0;
    }
    return usage();
}
