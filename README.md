# Digimon World 3 Recomp

A static recompilation of *Digimon World 3* (PS1, USA) to native x86_64 and ARM64. It targets
Vulkan, Metal and Direct3D 12 through [plume](https://github.com/renderbag/plume).

> **Status:** work in progress. The game boots and is playable on Linux, macOS (Apple silicon) and Windows, with
> a Metal, Vulkan or Direct3D 12 renderer and a software fallback. Expect rough edges.

You must supply your own copy of the game. No game data is included in this repository.

## Building

Every platform needs three steps, in this order. **Step 2 is required**: the game executable cannot be built
without the recompiled sources, which are generated from your own disc and never committed.

1. **Put your disc image** at `game/Digimon World 3 (USA).bin` / `.cue` (git-ignored), or point `-DDW3_GAME_DIR=` at
   its directory.
2. **Generate the recompiled code** (once, and again whenever `config/dw3.toml` or the recompiler changes). From the
   repository root, with `<preset>` as in the table below:

   ```sh
   cmake --preset <preset>
   cmake --build --preset <preset> --target dw3extract psxrecomp
   build/<preset>/tools/dw3extract/dw3extract "game/Digimon World 3 (USA).cue" extract game/extracted
   cmake --build --preset <preset> --target recompile      # writes generated/
   ```

   The generated C is architecture-neutral, so it can be produced on one machine and copied to `generated/` on another.
3. **Build and test**. CMake only picks up `generated/` when it configures, so configure again after step 2:

   ```sh
   cmake --preset <preset>
   cmake --build --preset <preset>
   ctest --preset <preset> -LE scenario -j8      # unit tests; `-L scenario` runs the in-game tests (needs the disc)
   ```

   The executable is `build/<preset>/app/dw3recomp`.

If the build stops with `fatal error: 'dw3/host.h' file not found`, `generated/` is missing: do step 2.

| Platform | Presets | Details |
|---|---|---|
| Linux x86-64 | `linux-x64-debug`, `linux-x64-release` | below |
| Linux arm64 | `linux-arm64-*` (native), `linux-arm64-cross-*` (from x86-64, tests under qemu) | [docs/BUILDING.md](docs/BUILDING.md) |
| macOS (Apple silicon) | `macos-arm64-debug`, `macos-arm64-release` | below |
| Windows x64 | `windows-x64-debug`, `windows-x64-release` (on Windows), `windows-x64-cross-*` (from Linux) | [docs/BUILDING.md](docs/BUILDING.md) |

### Linux

Requirements: clang ≥ 17, lld, CMake ≥ 3.24, Ninja, glslang, SDL3 and the Vulkan headers/loader. GoogleTest, toml++,
zstd and plume are fetched automatically if they are not installed.

```sh
# On immutable distros (Bazzite/Silverblue) use a distrobox:
distrobox create -n dw3-build -i registry.fedoraproject.org/fedora:latest
distrobox enter dw3-build -- sudo dnf install -y clang lld cmake ninja-build glslang SDL3-devel \
    vulkan-loader-devel vulkan-headers gtest-devel gmock-devel
```

Then run the three steps above with `<preset>` = `linux-x64-release` (inside the container if you use one).

### macOS

Requirements: Xcode command line tools (Apple clang), CMake ≥ 3.24, Ninja, glslang and SDL3.

```sh
brew install cmake ninja glslang sdl3        # spirv-tools is optional (shader validation)
```

Then run the three steps above with `<preset>` = `macos-arm64-release`. Rendering uses Metal. To build the `.app` and
disk image see [packaging/macos/README.md](packaging/macos/README.md).

### Windows and other targets

See [docs/BUILDING.md](docs/BUILDING.md) for Windows (native and cross-compiled from Linux, including running under
Wine), Linux arm64 cross builds and packaging.

## First run

The program ships without game data. On first start (or whenever no valid disc is configured) a setup window asks for
your own disc image of Digimon World 3 (USA): choose its `.cue`, the program checks the serial (SLUS-01436) and the
SHA-256 of the game executable, and saves the path as `system.disc_path` in `settings.toml`. `--setup` opens the window
again. `--disc PATH` overrides the saved path for one run.

The window also offers HD backgrounds, which is optional and off until you accept. Accepting downloads the
realesrgan-ncnn-vulkan upscaler (v0.2.0, MIT, about 2 to 9 MB depending on the platform) from its official release and
the converted Real-ESRGAN model (BSD 3-Clause) from this project's releases, using the `curl` that comes with your
system. Every file is checked against a fixed SHA-256, unpacked into `<user data>/tools`, and then a texture pack is
built from your disc on your own computer (this takes a long time and needs a Vulkan GPU; it can be cancelled and
resumed). If `curl` is missing the window lists the addresses and the folder to put the files in. Options that matter
while playing stay in the game's own settings screen. See `docs/SETTINGS.md` for details and
`tools/models/` for how the model files are made.

## Game files

Put your dump at `game/Digimon World 3 (USA).bin` / `.cue`. That directory is git-ignored. Tests
that need the disc are skipped when it is absent. `dw3extract` also lists and extracts the disc contents:

```sh
build/<preset>/tools/dw3extract/dw3extract "game/Digimon World 3 (USA).cue" extract game/extracted
```

## Layout

| Path | Contents |
|---|---|
| `tools/psxdisc` | cue/bin, raw sector, ISO9660 and PS-X EXE library |
| `tools/dw3extract` | disc file lister/extractor (includes the hidden `AAA/` tree) |
| `tools/psxrecomp` | MIPS R3000A → C static recompiler |
| `runtime/` | PS1 hardware and BIOS runtime, platform abstraction |
| `app/` | game executable (window, input, settings, setup wizard, presenters) |
| `host/` | boot/run logic shared by the app and the scenario runner (built once `generated/` exists) |
| `render/` | GPU renderers (Metal, Vulkan, Direct3D 12) and the software rasterizer |
| `config/` | recompiler configuration for DW3 |
| `tests/` | GoogleTest unit and integration tests, plus in-game scenarios |
| `packaging/` | macOS `.app`/dmg scripts and the Windows package note |
| `docs/` | architecture notes |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Options, visual presets, hotkeys and fast-forward are
described in [docs/SETTINGS.md](docs/SETTINGS.md).

## License

GPL-3.0. See [LICENSE](LICENSE).
