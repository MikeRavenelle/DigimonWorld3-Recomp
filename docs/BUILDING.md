# Building

## Linux

See the README. Presets: `linux-x64-debug`, `linux-x64-release`.

## Recompiled code

The game executable and the in-game tests need the recompiled sources, which are generated from your own disc and
never committed:

```sh
build/linux-x64-release/tools/psxrecomp/psxrecomp config/dw3.toml emit --out generated
```

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
