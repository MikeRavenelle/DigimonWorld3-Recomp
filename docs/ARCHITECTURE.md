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

## Rendering

The GPU is emulated at the command level (GP0/GP1, VRAM 1024×512×16) with a hardware renderer
on plume, following Beetle-PSX-HW's design. This is the basis for internal-resolution upscaling,
texture filtering, widescreen hacks, HDR output and DLSS/MetalFX later on. A software reference
rasterizer is kept for golden-image tests and headless runs.

`render/` holds the GPU renderer (`HwRenderer`, a `Rasterizer`). VRAM lives in two RGBA8 textures (5-bit channels
expanded, mask bit in alpha): one drawn into and a sampled copy refreshed only where a draw reads something the GPU
wrote since. The fragment shader reproduces the PS1 pixel path on integers (CLUT/texture-window fetch,
modulation, dithering, 5-bit blending, mask bits), so output matches the software rasterizer except for GPU
triangle edge/interpolation rounding. The CPU-side VRAM array stays authoritative for transfers: the Gpu calls
`vram_written` after changing it and `vram_read_begin` before reading it, which uploads or reads back the affected
regions. Only the Vulkan backend on Linux is built; plume's Metal and D3D12 backends are enabled per platform in
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
