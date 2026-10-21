# Building

## Linux

See the README (Linux section and the three build steps). Presets: `linux-x64-debug`, `linux-x64-release`.

## Linux arm64

The `linux-arm64-debug` and `linux-arm64-release` presets build natively on an aarch64 Linux machine, exactly like the
x64 presets (same packages, same steps). The `linux-arm64-cross-debug` and `linux-arm64-cross-release` presets
cross-compile from an x86-64 Linux host and run the tests under qemu-user.

### Cross toolchain

Needs `clang`, `lld`, CMake, Ninja, glslang and `qemu-user` (`qemu-aarch64` or `qemu-aarch64-static` on the `PATH`),
plus an aarch64 sysroot. On Fedora the sysroot can be created without an aarch64 machine:

```sh
sudo dnf install -y --forcearch=aarch64 --installroot=$HOME/.local/share/dw3-sysroot-aarch64 \
    --use-host-config --releasever=$(rpm -E %fedora) --setopt=install_weak_deps=False \
    glibc-devel libstdc++-devel gcc SDL3-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel \
    libXi-devel libXfixes-devel libXScrnSaver-devel libxkbcommon-devel wayland-devel vulkan-loader-devel \
    alsa-lib-devel pulseaudio-libs-devel systemd-devel libdrm-devel mesa-libgbm-devel mesa-libEGL-devel
sudo chown -R $USER: $HOME/.local/share/dw3-sysroot-aarch64
```

`cmake/toolchains/linux-arm64.cmake` reads `DW3_ARM64_SYSROOT` (environment or `-DDW3_ARM64_SYSROOT=`;
`~/.local/share/dw3-sysroot-aarch64` is the default). The sysroot supplies glibc, libstdc++ and SDL3; it is never part
of the repository.

```sh
cmake --preset linux-arm64-cross-release
cmake --build --preset linux-arm64-cross-release
ctest --preset linux-arm64-cross-release -LE scenario -j4    # unit tests under qemu
ctest --preset linux-arm64-cross-release -L scenario -j4     # in-game scenarios under qemu (needs the recompiled code and the disc)
```

The differential emitter test uses the generator from `build/linux-x64-release/tests/emit_testgen`
(`-DDW3_HOST_EMIT_TESTGEN=` to change it), so build the x64 release preset first, as for the Windows cross build.
Emulation is slow: allow a long `--timeout` for the scenarios. There is no GPU under qemu-user, so the renderer tests
that need Vulkan skip themselves and the software rasterizer is what runs.

On an aarch64 host, `cmake --preset linux-arm64-release` builds with the system compiler and libraries.

## macOS (arm64)

The `macos-arm64-debug` and `macos-arm64-release` presets build natively on an Apple silicon Mac with Xcode's Apple
clang. Needs the Xcode command line tools, CMake, Ninja, glslang and SDL3 (Homebrew: `brew install cmake ninja glslang
sdl3`; `spirv-tools` adds shader validation). GoogleTest, toml++, zstd, plume and SPIRV-Cross are fetched when CMake
configures; SDL3 is fetched and built too when it is not installed.

```sh
cmake --preset macos-arm64-release -DDW3_GAME_DIR=$HOME/dw3/game    # directory with the disc image
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-debug -LE scenario -j8     # unit tests
ctest --preset macos-arm64-release -L scenario -j4    # in-game scenarios (needs the recompiled code and the disc)
```

`DW3_GAME_DIR` defaults to `game` in the source tree. The recompiled code (see below) is architecture-neutral, so it can
be generated on another machine and copied to `generated/`; configure again after it appears.

The GPU renderer runs on plume's Metal backend (`--renderer metal`; `auto` and `gpu` select it too). The shaders are
GLSL compiled to SPIR-V with glslang, converted to Metal Shading Language with SPIRV-Cross (`tools/shaders`) and
embedded as text. Metal compiles them when the device is created, with fast math off, so the build needs no Metal
toolchain from Xcode. The renderer tests (`test_render`) compare Metal against the software rasterizer at every internal
resolution and option, as they do for Vulkan and Direct3D 12. plume is not built with MoltenVK, so `--renderer vulkan`
is unavailable on macOS, and without a usable Metal device `auto` falls back to the software path.

The window is high pixel density: window sizes in the settings are points, and the swap chain and the present
rectangle use the pixel size. Fullscreen borderless is macOS's own fullscreen (a separate Space); exclusive switches the
display mode.

Packaging: `packaging/macos` builds the `.app`, signs it, makes the disk image and notarizes it (see its README). For
a package, configure with `-DDW3_FETCH_SDL=ON` so that SDL is linked statically.

Exceptions thrown through the recompiled C (the stop request, a restored save state) rely on the C sources being built
with `-fexceptions`, which `dw3_generated` does; the save-state scenarios exercise it.

Memory cards, save states and the disc index cache are under `~/Library/Application Support/DW3Recomp`. The window is
created at its logical size and the swap chain follows the Retina backing size. The app bundle, signing and
notarization are described in `packaging/macos/README.md`.

## Recompiled code

The game executable and the in-game tests need the recompiled sources, which are generated from your own disc and
never committed:

```sh
build/<preset>/tools/dw3extract/dw3extract "game/Digimon World 3 (USA).cue" extract game/extracted
cmake --build --preset <preset> --target recompile     # runs psxrecomp config/dw3.toml emit --out generated
```

`config/dw3.toml` reads the extracted files from `game/extracted`, so extract first. Without `generated/` CMake skips
`host/` and the app fails to compile (`'dw3/host.h' file not found`). Configure again after `generated/` appears.
Build the recompiler natively first. The Windows build compiles the same generated C.

## Windows (cross-compiled from Linux)

The `windows-x64-cross-debug` and `windows-x64-cross-release` presets build Windows x64 executables on a Linux host
with clang-cl and lld-link against the Windows SDK and the MSVC CRT. The CRT is linked statically and SDL3 is built
from source and linked statically, so the executables need no DLLs next to them. (The `windows-x64-debug` preset is
for building on Windows itself.)

### Toolchain

Needs `clang` with the clang-cl driver, `lld`, `llvm` (for `llvm-lib`, `llvm-rc`, `llvm-mt`), CMake, Ninja and glslang
(`glslangValidator`/`glslang`; `spirv-val` is used when present).

The SDK and CRT come from [xwin](https://github.com/Jake-Shadle/xwin) (MIT/Apache-2.0), which downloads them from
Microsoft; running it means accepting Microsoft's license:

```sh
xwin --accept-license splat --output ~/.local/share/xwin
export DW3_XWIN_DIR=~/.local/share/xwin     # optional: this is the default location
```

`cmake/toolchains/windows-x64-clang-cl.cmake` reads `DW3_XWIN_DIR` (environment or `-DDW3_XWIN_DIR=`).

### Shaders for Direct3D 12

The GLSL shaders are compiled to SPIR-V, converted to HLSL with SPIRV-Cross and compiled to DXIL with DXC
(`cmake/D3D12Tools.cmake`). Both tools are found on the `PATH` or given with `-DDW3_SPIRV_CROSS=` and `-DDW3_DXC=`;
otherwise SPIRV-Cross is built from source for the build machine and the DXC release binaries are downloaded when
CMake configures.

### Build

```sh
cmake --preset windows-x64-cross-release
cmake --build --preset windows-x64-cross-release
```

Dependencies (plume, SDL3, toml++, zstd, GoogleTest) are fetched and cross-built. The differential emitter test needs
its generator to run on the build machine, so it uses `build/linux-x64-release/tests/emit_testgen`
(`-DDW3_HOST_EMIT_TESTGEN=` to change it); build the Linux release preset first.

### Running under Wine

`tools/wine/run-wine.sh` runs an executable under the Wine of a Proton build, with vkd3d-proton supplying Direct3D 12 and
DXVK's DXGI. It finds Proton-GE in `~/.steam/steam/compatibilitytools.d` or Steam's Proton, or uses `DW3_PROTON`
(the directory that contains `files/bin/wine`), and creates a Wine prefix on first use (`DW3_WINEPREFIX`). Run it on
the host, outside any container, so that Wine can reach the GPU.

```sh
tools/wine/run-wine.sh build/windows-x64-cross-release/tests/test_render.exe
tools/wine/run-wine.sh build/windows-x64-cross-release/app/dw3recomp.exe \
    --disc "game/Digimon World 3 (USA).cue" --renderer d3d12 --frames 400 --screenshot Z:/tmp/title.bmp
```

Output paths inside Wine are Windows paths: `Z:` is the root of the Linux file system. The test executables register
with CTest as one test each (they cannot be run at build time to list their cases).

`--renderer` takes `auto` (Direct3D 12, then Vulkan, then software on Windows), `gpu` (the same without the software
fallback), `d3d12`, `vulkan` or `software`. The renderer tests (`test_render`) run each case on every backend the build
contains, against the software rasterizer; select one with, for example,
`--gtest_filter='*/D3D12_*'`.

### Tests: build in the container, run on the host

The cross toolchain sets `CMAKE_CROSSCOMPILING_EMULATOR` to `tools/wine/run-wine.sh`, so CTest runs every test
executable under Wine. When the script is started inside a distrobox/toolbox container it runs itself again on the host
with `distrobox-host-exec`, because Wine needs the host's GPU and audio. The build and `ctest` therefore both run in
the build container:

```sh
distrobox enter dw3-build -- cmake --build --preset windows-x64-cross-release
distrobox enter dw3-build -- ctest --preset windows-x64-cross-release -LE scenario -j4    # unit tests
distrobox enter dw3-build -- ctest --preset windows-x64-cross-release -L scenario -j4     # in-game scenarios
```

Only Wine and a Proton build (see above) are needed on the host. Paths that CTest passes to the executables (the
disc, the scenario files, the state directory) are given as `Z:/...` paths. On a machine without containers the same
commands work directly.

To use a different Wine, configure with `-DCMAKE_CROSSCOMPILING_EMULATOR=<command>`.

### Native Windows host

The `windows-x64-debug` and `windows-x64-release` presets build with clang-cl on Windows itself. Dependencies are the
same: SDL3 and GoogleTest are always built from source on Windows, glslang must be on the `PATH`, and the DXC release
for Windows is downloaded at configure time (SPIRV-Cross is built from source). The static CRT is selected for every
Windows build.

### Where files go on Windows

Memory cards, save-state slots and the disc index cache are under `%APPDATA%\DW3Recomp` (`memcards`, `states`,
`cache`). Command-line paths, including the disc, may contain spaces and non-ASCII characters; they are handled as
UTF-8 and converted to wide paths at the file system boundary.

### Window and audio checks

`--window-size WxH`, `--fullscreen`, `--resize-at FRAME WxH` and `--fullscreen-at FRAME` exercise the window and the
swap chain in audit runs. `--audio` with `--frames` plays audio in real time and logs the device buffer underruns and
overrun drops when the run ends.

### Packaging

```sh
distrobox enter dw3-build -- cmake --install build/windows-x64-cross-release --component app --prefix out/dw3recomp
distrobox enter dw3-build -- cpack --config build/windows-x64-cross-release/CPackConfig.cmake
```

The ZIP holds `dw3recomp.exe` (static CRT and SDL, no DLLs), `LICENSE` and a `README.txt` that explains how to supply
the disc. It contains no game data and no SDK files.

## Release packages

A release has no game code. `cmake --install <build dir> --component app --prefix <dir>` puts the program, the licence
and `toolchain/` (the headers and entry point a game module is built from) in place; add `-DDW3_ZIG_DIR=<unpacked zig>`
when configuring to include the compiler (`.github/workflows/release.yml` pins the zig version and checks its SHA-256).
To try the player's path from a checkout, build without `generated/` (or move it away), point `DW3_TOOLCHAIN` at a folder
laid out like `toolchain/`, and run `dw3recomp --prepare --disc "<disc.cue>"`; the module lands in the user data folder
under `module/`. The setup window does the same on first start.
