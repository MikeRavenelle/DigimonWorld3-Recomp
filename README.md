# Digimon World 3 Recomp

Digimon World 3 (PS1, USA) running as a native program on your PC. It isn't an emulator. The game's code was
translated ahead of time into code your machine runs directly, so it starts fast, sits well on handhelds and gives us
room to do things an emulator can't do cleanly.

You bring your own copy of the game. Nothing from the disc is in this repo or in any download from it.

> **Status:** beta. The whole game boots and plays on Linux, macOS (Apple silicon) and Windows. Expect rough edges,
> and please report them.

## What you get

**A picture that doesn't look 25 years old**
- Three looks: **Original** (exactly what the PlayStation showed), **Enhanced** (same look, sharper, no dither, no
  wobbling polygons) and **Remaster** (everything cranked). One key flips between Original and your pick so you can
  compare.
- 3D rendered at up to 8x the original resolution.
- HD backgrounds. The field maps are pre-rendered pictures, so we can swap in AI-upscaled ones. You make them from your
  own disc on your own PC, the setup walks you through it, and it's optional.
- Seamless maps. No more dark squares popping in at the edges of a field while you walk.
- Widescreen at 16:9, 16:10 or 21:9 without stretching the picture.
- HDR, FSR, SMAA, sharpening, colour grading, and a CRT look if you want scanlines.

**Play it your way**
- Save states anywhere, on top of the normal memory card saves (those are plain files you can back up).
- Fast-forward, or run the whole game at 2x to 4x with the music staying in tune.
- Text speed up to instant, encounter rate (half, double or off), an XP multiplier and a god mode. All of it is off by
  default, so the game plays like the original until you say otherwise.
- Separate music and effects volume.
- Controller support with Xbox, PlayStation or Nintendo button prompts, and everything is rebindable.
- Borderless, windowed or exclusive fullscreen on whichever monitor you pick.

Every option lives in an in-game settings screen, and in a plain `settings.toml` if you like editing files. The full
list is in [docs/SETTINGS.md](docs/SETTINGS.md).

## Running it

1. Get a dump of your own disc as a `.cue` / `.bin` pair.
2. Start the program. First launch opens a setup window that asks for the `.cue`, checks it's the right game
   (SLUS-01436) and remembers it. Then it builds the game from your disc on your PC. That takes a minute or a few, it
   only happens once, and nothing gets downloaded for it. `--setup` opens the window again.
3. Optional: say yes to HD backgrounds in the same window. It downloads an upscaler and a model, checks them against
   fixed hashes, and builds a pack from your disc. That takes a long time and needs a Vulkan GPU, but you can cancel and
   pick it up later.

Prebuilt downloads aren't up yet. Until they are, you build it yourself (below).

## Under the hood

The recompiler reads the main executable and all 263 overlays off the disc, finds every function, and writes C for
each one. That C is compiled for your platform and linked against a runtime that does the rest of the console: GPU,
SPU, GTE, CD drive, memory cards, timers, DMA and a native BIOS. No CPU emulation, no interpreter, and no fallback
if the recompiler missed something. A missing function stops the game with a clear message.

- Renderers: Metal, Vulkan and Direct3D 12 through [plume](https://github.com/renderbag/plume), plus a software
  rasterizer. All 3D and post-processing happens on the GPU.
- Platforms: Linux x86-64 and arm64, macOS on Apple silicon, Windows x64.
- Save states record where the game is in the original's addresses, so they don't depend on which build made them.
- The generated C is architecture neutral, so one machine can produce it for the others.
- Licensed GPL-3.0.

### Building

The recompiled code comes from your disc, so it's never committed and you generate it before building the game.

1. Put your disc at `game/Digimon World 3 (USA).bin` / `.cue` (git-ignored), or point `-DDW3_GAME_DIR=` at its folder.
2. Generate the recompiled code. Do this once, and again when `config/dw3.toml` or the recompiler changes:

   ```sh
   cmake --preset <preset>
   cmake --build --preset <preset> --target dw3extract psxrecomp
   build/<preset>/tools/dw3extract/dw3extract "game/Digimon World 3 (USA).cue" extract game/extracted
   cmake --build --preset <preset> --target recompile      # writes generated/
   ```

3. Configure again so CMake sees `generated/`, then build and test:

   ```sh
   cmake --preset <preset>
   cmake --build --preset <preset>
   ctest --preset <preset> -LE scenario -j8      # unit tests; -L scenario runs the in-game tests and needs the disc
   ```

The program ends up at `build/<preset>/app/dw3recomp`.

| Platform | Presets |
|---|---|
| Linux x86-64 | `linux-x64-debug`, `linux-x64-release` |
| Linux arm64 | `linux-arm64-*` (native), `linux-arm64-cross-*` (from x86-64, tests under qemu) |
| macOS (Apple silicon) | `macos-arm64-debug`, `macos-arm64-release` |
| Windows x64 | `windows-x64-debug`, `windows-x64-release` (on Windows), `windows-x64-cross-*` (from Linux) |

You need clang, CMake 3.24 or newer, Ninja, glslang and SDL3. On Linux you also need the Vulkan headers and loader.
GoogleTest, toml++, zstd and plume are fetched for you. On Bazzite or Silverblue do all of this in a distrobox. macOS and
Windows details, cross builds and packaging are in [docs/BUILDING.md](docs/BUILDING.md) and
[packaging/macos/README.md](packaging/macos/README.md).

Without `generated/` everything else still builds and the unit tests run (that is what CI does), but there's no game
code in the program. It needs a game module instead.

### Game modules

The recompiled code can also be built as a shared library and loaded at run time, with `--module PATH`. The library
gets the runtime's services through a small table when it loads, so it has no link-time dependency on the program and
loads the same way on every OS. `-DDW3_BUILD_GAME_MODULE=ON` builds one from `generated/`.

This is how releases work. The packages contain the program and a small toolchain (a `zig` compiler plus two source
files) and no game code. The setup recompiles your disc with the same recompiler as the build above, compiles the
result with that toolchain and keeps the module in your user data folder, named after the recompiler version and the
disc. `dw3recomp --prepare` does the same from the command line. Tagging `v*` runs the release workflow, which builds
and tests every platform and publishes the packages as a pre-release.

### Layout

| Path | Contents |
|---|---|
| `tools/psxdisc` | cue/bin, raw sector, ISO9660 and PS-X EXE library |
| `tools/dw3extract` | disc lister and extractor, including the hidden `AAA/` tree |
| `tools/psxrecomp` | MIPS R3000A to C recompiler |
| `tools/texpack` | texture pack builder for HD backgrounds |
| `runtime/` | console hardware, BIOS, game module loader, platform layer |
| `app/` | the program: window, input, settings, setup window, presenters |
| `host/` | boot and run logic shared by the app and the scenario runner |
| `render/` | GPU renderers and the software rasterizer |
| `config/` | recompiler configuration for DW3 |
| `tests/` | unit tests and in-game scenarios |
| `packaging/` | macOS `.app` and dmg scripts, Windows and Linux packaging |
| `docs/` | architecture, building and settings |

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

GPL-3.0. See [LICENSE](LICENSE).
