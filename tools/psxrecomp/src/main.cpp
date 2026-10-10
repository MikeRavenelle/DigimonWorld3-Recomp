// psxrecomp: static recompiler, MIPS R3000A -> C.
// Pipeline (see docs/ARCHITECTURE.md): load config -> load exe + overlays -> decode ->
// function discovery -> per-function C emission -> function/overlay lookup tables.

#include <cstdio>

#include <toml++/toml.hpp>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: psxrecomp <config.toml>\n");
        return 2;
    }
    try {
        auto cfg = toml::parse_file(argv[1]);
        std::printf("psxrecomp: loaded config for '%s' (emission not implemented yet)\n",
                    cfg["game"]["name"].value_or("?"));
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }
    return 0;
}
