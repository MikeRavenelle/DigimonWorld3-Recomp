# Digimon World 3 Recomp

A static recompilation of *Digimon World 3* (PS1, USA) to native x86_64 and ARM64. It targets
Vulkan, Metal and Direct3D 12 through [plume](https://github.com/renderbag/plume).

> **Status:** early scaffolding. The game does not run yet.

You must supply your own copy of the game. No game data is included in this repository.

## Building (Linux)

Requirements: clang ≥ 17, CMake ≥ 3.24, Ninja, SDL3, Vulkan headers/loader. GoogleTest and
toml++ are fetched automatically if they are not installed.

```sh
# On immutable distros (Bazzite/Silverblue) use a distrobox:
distrobox create -n dw3-build -i registry.fedoraproject.org/fedora:latest
distrobox enter dw3-build -- sudo dnf install -y clang lld cmake ninja-build SDL3-devel \
    vulkan-loader-devel vulkan-headers gtest-devel gmock-devel

cmake --preset linux-x64-debug
cmake --build --preset linux-x64-debug
ctest --preset linux-x64-debug
```

## Game files

Put your dump at `game/Digimon World 3 (USA).bin` / `.cue`. That directory is git-ignored. Tests
that need the disc are skipped when it is absent.

```sh
build/linux-x64-debug/tools/dw3extract/dw3extract "game/Digimon World 3 (USA).cue" extract game/extracted
```

## Layout

| Path | Contents |
|---|---|
| `tools/psxdisc` | cue/bin, raw sector, ISO9660 and PS-X EXE library |
| `tools/dw3extract` | disc file lister/extractor (includes the hidden `AAA/` tree) |
| `tools/psxrecomp` | MIPS R3000A → C static recompiler |
| `runtime/` | PS1 hardware and BIOS runtime, platform abstraction, renderer |
| `app/` | game executable |
| `config/` | recompiler configuration for DW3 |
| `tests/` | GoogleTest unit and integration tests |
| `docs/` | architecture notes |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Options, visual presets, hotkeys and fast-forward are
described in [docs/SETTINGS.md](docs/SETTINGS.md).

## License

GPL-3.0. See [LICENSE](LICENSE).
