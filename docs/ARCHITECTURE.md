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
code is involved, and with the option off nothing is recorded. Only the Vulkan backend on Linux is built; plume's Metal and D3D12 backends are enabled per platform in
`cmake/Platform.cmake`. plume is fetched at a pinned commit with `cmake/patches/plume-vulkan.patch` applied.

## Platforms

| Target | Status | Backend |
|---|---|---|
| Linux x86_64 | primary | Vulkan |
| macOS arm64 | second | Metal |
| Linux arm64 | planned | Vulkan |
| Windows x64 | planned | D3D12 / Vulkan |

Arch- and OS-specific code lives only in `runtime/platform` and `cmake/Platform.cmake`.
Generated C is architecture-neutral.

## Licensing

GPL-3.0. Code may be adapted from GPL-compatible projects (PCSX-Redux, Beetle PSX/mednafen,
OpenBIOS). **DuckStation (CC-BY-NC-ND) and PSXRecomp (PolyForm-NC) must not be copied.** No game
data, extracted assets or generated C may be committed.
