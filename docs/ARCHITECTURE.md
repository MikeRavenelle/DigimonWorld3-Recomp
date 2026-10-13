# Architecture

DW3Recomp is a **pure static recompilation** of *Digimon World 3* (USA, SLUS-01436) for the
PlayStation. Every piece of MIPS R3000A code the game can run is translated ahead of time to C and
compiled natively. There is no interpreter or JIT fallback: reaching un-recompiled code is a
fatal, diagnosable error.

```
 disc.bin/cue ──► dw3extract ──► SLUS_014.36 + 263 *.PRO overlays + data files
                                     │
                     config/dw3.toml │ (function hints, overlay bases, patches)
                                     ▼
                                 psxrecomp ──► generated/*.c + function/overlay tables
                                                    │
         runtime (C++20) ◄──────────────────────────┘
           core/     bus, context, function lookup, scheduler/events
           bios/     HLE kernel (A0/B0/C0 tables, exceptions, events)
           hw/       GPU, GTE, SPU, CD-ROM, MDEC, DMA, timers, IRQ, SIO/pad, memcard
           render/   PS1 GPU command stream ──► renderer interface ──► plume (Vulkan/D3D12/Metal)
           platform/ OS + arch abstraction, SDL3 window/input/audio
         app/        executable, settings UI, QOL features
```

## Disc layout (USA)

| Region | LBA | Notes |
|---|---|---|
| Visible ISO9660 root | 22 | SYSTEM.CNF, SLUS_014.36 (LBA 24), DUMMY (35 MB padding) |
| Hidden `AAA/` tree | 473 | Real directory records, not linked from the root. 2273 files. |
| `AAA/PRO/*.PRO` | 483–1610 | 263 raw code overlays (no header) |
| `AAA/DAT/**` | … | Models, stages, effects, sound, per-language text |
| `AAA/STR/*.STR` | ≈141859+ | FMV with interleaved XA audio |

The game also hard-codes LBA tables in the exe (around `0x800450D4`), so the CD layer serves raw
LBAs. It does not resolve file paths.

## Memory map / overlays

- Main exe: text `0x80010000`–`0x800F0000`, entry `0x80010EC4`, stack `0x801FFF00`.
- crt0 clears BSS `0x8005C480`–`0x80082448`; `$gp = 0x8005C2F8`; `main` = `0x800144FC`.
- Mode overlays (FIELDSTG, FIGHTSTG, CARDGAME, ST*…) load at `0x80082448` (end of BSS).
- Stage/battle sub-overlays (WSTAG*, WFIGHT*) load at about `0x800A4CA4`. The exe image also has
  data there (`0x800A4CA4`–`0x800EFDA4`), so the initial contents of that slot are still being investigated.
- Exact bases and the slot hierarchy are determined by tracing the loader (tracked task).

Recompiled functions are keyed by `(overlay_id, vaddr)`. The runtime tracks which overlay is
resident in each slot by hooking the game's overlay loader. Indirect jumps (`jr`, `jalr`, function
pointers) resolve through `dw3_lookup_func`, which consults the resident overlay set.

## Generated code contract

See `runtime/include/dw3/recomp.h`.
- One C function per MIPS function: `void f_<ovl>_<addr>(RecompContext*)`.
- Delay slots are emitted in order. Load-delay hazards are detected by the recompiler and
  reported. Real hazards get patched in config.
- RAM and scratchpad accesses are inlined fast paths. Everything else goes to `dw3_mem_read/write`.
- Each basic block adds its cycle cost to `ctx->cycles`. On backward branches and calls, the
  generated code calls `dw3_service_events` once `cycles >= next_event`. That call delivers
  VBlank, timer, CD and DMA interrupts by running the HLE exception handler, which in turn calls
  the game's recompiled callbacks.
- COP2 (GTE) ops call into `hw/gte`.
- Each function with call sites or loops starts with a dispatch on `ctx->resume`, and each segment has a
  table of those labels (`RecompSegment::resume`): the return point of every call and the target of every
  backward jump. Event checks record the label they are about to jump to in `ctx->cur_pc`. Save states
  resume through these (below). A `jr $ra` whose delay slot writes `$ra` is a recompile error.

## Host overrides

`Machine::override_function(addr, fn)` makes everything that resolves through `Machine::lookup` (indirect calls, `jr`
targets, task and window method tables) run a host routine instead of the recompiled function at `addr`; the routine
can call the original through `Machine::lookup_original`. Calls the recompiler resolved statically do not go through
the lookup, so only functions reached indirectly can be overridden. An override is still ordinary recompiled-style
code: it reads and writes guest memory, calls recompiled functions with `dw3_call` and returns to its caller, with no
interpreter involved.

Overrides keep their state in guest memory where they can. A save state then carries it, and loading one never leaves
a host flag disagreeing with the game. The in-game settings screen (`app/ingame`) is built this way. It overrides the
per-frame function of the field START menu's list (`0x800126FC`): while the list waits for input, SELECT writes a marker
into the menu task and the override replaces the list's rows and header text, through the windows' own text method,
with the settings. The menu's code still runs every frame to draw its panels, with the pad's newly-pressed words
cleared so that it sees no input. Closing the screen puts the original texts back.

## Save states

A state is guest state only: RAM, scratchpad, CPU and GTE registers, the scheduler queue and every hardware
block, the native BIOS tables, and one guest PC. Nothing in it depends on how the recompiler laid out the
host code, so a state loads in any build of the same game whose tables have a label at that PC (the
return points of calls exist in every build).

Each block describes its state once in an `io(StateIo&)` method used for both directions
(`runtime/include/dw3/state.h`). Data is little-endian and fixed width, grouped in sections that carry a tag,
a layout version and a length. A reader skips sections it does not know; a block that gains a field bumps its
version and reads the field with `io_since`, so older states load with a default. Overlays are recorded by
name. Memory cards are not stored, only a hash of each, and a load warns when they differ. The file is a
header (magic, container version, encoding, a hash of the game's boot executable, sizes, CRC32) followed by
the zstd-compressed payload. Stretches of the payload that also occur on the disc (most of RAM and SPU RAM
is data read from it) are replaced by references into the disc before compression (`disc_ref.h`).

The game's call stack is the host stack, but the guest call chain is also in guest memory, so only the
innermost function is resumed. A state is taken at a safe point: the event check of a backward jump, after
due events and interrupts were handled, with no native kernel frame active (`Bios::host_depth() == 0`). The
game spins in VSync/DrawSync loops, so one comes up within a frame or so; `Host::save_state` queues the
capture for it. Loading throws out of the safe point to `Host::run`, which calls `Machine::resume`: it finds
the function that has a label at the saved PC, enters it with `ctx->resume` set, and when it returns
continues at the address in `$ra` the same way, until the guest returns to 0. A state taken while an
interrupt handler or event callback runs is refused; the next frame's safe point is used instead.

Scenario tests use states to skip long prefixes: `[[savestate]]` writes one, `start_state` or
`[[loadstate]]` restores one, and `replay = true` re-runs the script from a saved state while checking
RAM, VRAM and CPU state frame by frame. States are written under the build directory and never committed.

## Rendering

The GPU is emulated at the command level (GP0/GP1, VRAM 1024×512×16) with a hardware renderer
on plume, following Beetle-PSX-HW's design. This is the basis for internal-resolution upscaling,
texture filtering, widescreen hacks, HDR output and DLSS/MetalFX later on. A software reference
rasterizer is kept for golden-image tests and headless runs.

`render/` holds the GPU renderer (`HwRenderer`, a `Rasterizer`). VRAM lives in two RGBA8 textures (5-bit channels
expanded, mask bit in alpha): one drawn into and a sampled copy refreshed only where a draw reads something the GPU
wrote since. The fragment shader reproduces the PS1 pixel path on integers (CLUT/texture-window fetch,
modulation, dithering, 5-bit blending, mask bits). Colour and texel coordinates are evaluated from the software
rasterizer's fixed-point planes, so native pixels match it exactly, except where the GPU's triangle edge rules differ.
True colour (`--true-color`) and texture filtering (`--texture-filter`) draw into a second, 16-bit display layer
beside the exact VRAM texture; the layer is only shown where its recorded VRAM word still matches, so everything the
game can read back or sample stays exact. The CPU-side VRAM array stays authoritative for transfers: the Gpu calls
`vram_written` after changing it and `vram_read_begin` before reading it, which uploads or reads back the affected
regions. `--precise-geometry` adds sub-pixel vertices and perspective-correct texturing for polygons the GTE projected
(RTPS/RTPT): the GTE records the position it had before truncating to integer SX/SY, keyed by the packed word, and the
Gpu attaches it to polygon vertices that still carry that word, falling back to the integer otherwise. No generated
code is involved, and with the option off nothing is recorded. Linux builds use plume's Vulkan backend; Windows builds contain both Vulkan and Direct3D 12
(`--renderer auto` prefers Direct3D 12 there); macOS builds use Metal. The shaders are written in GLSL and compiled to
SPIR-V with glslang; for Direct3D 12 the SPIR-V is converted to HLSL with SPIRV-Cross and compiled to DXIL with DXC, for
Metal it is converted to Metal Shading Language with SPIRV-Cross (`tools/shaders/spirv_to_msl.cpp`, which lays resources
out the way plume's Metal backend binds them: descriptor sets as argument buffers at buffer(N), push constants at
buffer(8)) and embedded as source that the device compiles with fast math off. All forms are embedded and chosen by the
backend at run time. Vertex inputs use `TEXCOORD<location>` semantics, which is what
SPIRV-Cross emits, and vertex shaders are flipped in y for Direct3D's and Metal's clip space. plume is fetched at a
pinned commit with `cmake/patches/plume-vulkan.patch`, `cmake/patches/plume-d3d12.patch` and
`cmake/patches/plume-metal.patch` applied (the last adds the texture-to-buffer copy and source-text shaders). The renderer tests run against
the software rasterizer on every backend the build contains, so all backends are held to the same pixels.

## Texture replacement

Higher-resolution backgrounds, built from the player's own disc (nothing derived from the game is distributed). The
code is in `render/src/texrep` (no GPU dependency, unit-tested), the draw path in `HwRenderer`, and the generator in
`tools/texpack`.

What the game does: a field background is cut into blocks of at most 128x128 texels, 8-bit indexed, each uploaded with
its own 256-entry palette by a CPU-to-VRAM transfer and drawn every frame as opaque textured rectangles that map texels
to pixels one to one (partial rectangles at the screen edge). The images never change after the upload; palettes of
animated parts are re-uploaded.

Identification. Every completed CPU upload becomes a block in `BlockRegistry` (rectangles smaller than 8x8 words or wider
than 128 words, such as palettes and state loads, are ignored; any overlapping upload, draw, fill or copy removes the
blocks it touches, so a block always describes unchanged CPU-side VRAM). A block's content hash (64 bit, shape included,
position independent) is computed the first time a sprite samples it. A pack entry is keyed by (block hash, palette
hash), the palette hash being taken over the 256 entries the sprite uses at draw time. A sprite that samples inside no
block (what a save state restores) makes an implicit block of its own rectangle if that overlaps no other block.

Drawing. Only opaque, unflipped 8-bit rectangles without a texture window are candidates. `split_by_blocks` cuts the
rectangle along block edges; each piece that has a pack entry is queued as its own rectangle with `state_bits::kHd`, the
pack scale in the state word and, in the vertex, the offset that maps texel coordinates of the texture page onto the
block's image in the replacement atlas. Pieces without an entry are drawn as before. Nothing is replaced if the GPU
has drawn into the palette row (the CPU copy might be stale). The replacement is only used by the display layer, so the
renderer is forced into layer mode when a pack is active: the exact target (what the game can read back) still receives
the original texels, the layer's colour comes from the atlas, bilinear-filtered when the internal resolution differs from
the pack scale (taps stay inside the piece, so neighbouring pieces never bleed), then modulated by the sprite colour
exactly like any texel (so the tinted second background layer and fades keep working), without 5-bit quantisation or
dithering. Transparent original texels still discard, so cut-outs keep their shape. The layer, being 16 bit per
channel, passes the colour through to the presenter, which already shows it where the recorded VRAM word still matches.
Without a pack nothing is tracked, no extra draw state is set and output is byte-identical to before (the draw pass
of the layer variant gains a third texture binding, a 1x1 dummy without a pack; the non-layer pipeline is unchanged).

Atlas. A 4096x4096 RGBA8 texture filled by a shelf packer on demand: the PNG of an entry is decoded and uploaded the first
time a sprite needs it. When the atlas is full it is cleared (after the draws that read it are recorded) and refilled as
needed. Images that fail to load are reported once and skipped.

Dumping. With `--dump-textures` the renderer records, per frame, which blocks were drawn with which palette and where
each block's corner lies on screen (the game's own screen coordinates, so the draw offset of the frame buffer cancels).
Blocks of one picture keep the same relative position in every frame, whatever the scroll; pairs that are always at the
same offset are merged into components (strongest evidence first; pairs that would make two blocks overlap, or that
change offset between frames as parallax layers do, are not merged). The dump holds every block/palette image decoded
to RGBA, and one image per component with the blocks pasted together.

Pack generation. For every dumped image `texpack` takes the component image, cuts out the block's rectangle plus 16
pixels around it (edge pixels repeated beyond the component), pastes the block with that palette over its place,
upscales that window with the external program (RGB only: the program mishandles alpha), crops the block's area from the
result and writes it. Context from the neighbouring blocks makes the block edges of a component continuous; where no
neighbour was seen the edge pixels repeat. A model whose factor is a multiple of the pack scale is reduced with a box
filter.

Limits: only the display layer path (GPU renderer) is supported; the software presenter ignores packs and dumps.
Polygons (quads used for objects) and semi-transparent or flipped sprites are not replaced. Palette animation is handled
by one image per observed palette state, and unseen states fall back to the original. Block and palette hashes cover
what the game holds in VRAM, so a pack is valid for any copy of the same game version.

## Post-processing

`render/src/post_chain.cpp` and `render/shaders/post/` hold the post-processing passes. They are enabled by `PostFx`
(`render/include/dw3/postfx.h`), which the presenter builds from the settings. With nothing enabled `present()` takes
the original path (the display area is scaled straight from VRAM) and its output is unchanged. Otherwise:

1. Extract: the display area is drawn 1:1 with nearest sampling into an image at the internal resolution (the
   display layer is honoured, so true colour and filtering show through).
2. SMAA 1x, three passes: colour edge detection, blending weights (with the precomputed area and search textures in
   `render/data`), neighbourhood blending.
3. CAS sharpening (skipped when the output scaler is FSR, whose RCAS step sharpens instead).
4. Colour grading.
5. Scaling into the picture rectangle of the window: nearest or bilinear; or FSR 1 EASU into a window-sized image
   followed by RCAS (only when the picture is enlarged on at least one axis, else bilinear).
6. CRT, last, at window resolution, reading the result of the step before (scanlines are tied to the console's own
   lines through the picture's height, not to the image's rows).

Steps 1 to 4 and the first part of 5 are rendered once per frame; the final pass is repeated for the window and for a
capture. All intermediate images use the swap chain's colour format, so a single pipeline per shader serves every
target; pipelines are created the first time a pass is used. Every shader goes through the same SPIR-V, DXIL and MSL
conversion as the rest. The SMAA search and area lookups and the FSR gather are done with explicit texel fetches,
because the passes read plain textures without samplers.

## Platforms

| Target | Status | Backend |
|---|---|---|
| Linux x86_64 | primary | Vulkan |
| macOS arm64 | builds and runs natively (see BUILDING.md) | Metal |
| Linux arm64 | builds (native and cross; see BUILDING.md) | Vulkan |
| Windows x64 | builds and runs (cross-compiled; see BUILDING.md) | D3D12 / Vulkan |

Arch- and OS-specific code lives only in `runtime/platform` and `cmake/Platform.cmake`.
Generated C is architecture-neutral.

C++ exceptions cross generated C frames (a fatal error in a test, the stop request, a restored save state). On
Windows the C++ code is therefore compiled with `/EHs`: `/EHsc` lets the compiler assume `extern "C"` functions never
throw, and a `catch` around a call into the runtime's C interface is then skipped.

## Licensing

GPL-3.0. Code may be adapted from GPL-compatible projects (PCSX-Redux, Beetle PSX/mednafen,
OpenBIOS). **DuckStation (CC-BY-NC-ND) and PSXRecomp (PolyForm-NC) must not be copied.** No game
data, extracted assets or generated C may be committed.

Third-party shader code (MIT, each file keeps its notice): SMAA by Jimenez et al. (`render/shaders/post/smaa_*`; the area
and search textures in `render/data` are generated data from the same reference implementation), AMD FidelityFX CAS
(`cas.frag`) and FSR 1 (`fsr_easu.frag`, `fsr_rcas.frag`). The CRT shader is original.

stb_image and stb_image_write (`third_party/stb`, public domain or MIT, by Sean Barrett) read and write the PNG files of
texture dumps and packs. The pack generator calls the external Real-ESRGAN ncnn Vulkan program (MIT; its model weights
come from the Real-ESRGAN project, BSD 3-Clause); neither is part of this repository or of the releases.
