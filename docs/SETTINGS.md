# Settings

Options live in `settings.toml` in the user data directory (the same place as memory cards and save states):

| Platform | Directory |
|---|---|
| Windows | `%APPDATA%\DW3Recomp` |
| macOS | `~/Library/Application Support/DW3Recomp` |
| Linux | `$XDG_DATA_HOME/DW3Recomp` (default `~/.local/share/DW3Recomp`) |

The file is created with the defaults, and comments explaining every key, on the first normal start. Missing keys use
their defaults, unknown keys are ignored, and an invalid value is reported in the log and replaced by its default.

Precedence: built-in defaults, then `settings.toml`, then command-line flags. Flags apply to that run only and are
never written to the file.

- `--config PATH` uses another settings file (created if missing).
- `--no-config` neither reads nor writes any settings file. Runs with `--frames` (audit and test runs) behave like
  `--no-config` unless `--config` is given.

## Options

Keys are grouped in `[graphics]`, `[audio]` and `[system]`. Settings marked "restart" take effect on the next start;
everything else applies immediately.

| Key | Values | Flag |
|---|---|---|
| `graphics.renderer` (restart) | `auto`, `gpu`, `vulkan`, `d3d12`, `metal`, `software` | `--renderer` |
| `graphics.preset` | `original`, `enhanced`, `remaster`, `custom` | `--preset` |
| `graphics.comparison_preset` | `enhanced`, `remaster` | |
| `graphics.internal_res` | 1 to 8 | `--internal-res` |
| `graphics.true_color` | bool | `--true-color` |
| `graphics.texture_filter` | `nearest`, `bilinear`, `xbr` | `--texture-filter` |
| `graphics.precise_geometry` | bool | `--precise-geometry` |
| `graphics.hd_backgrounds` | bool | `--hd-backgrounds` |
| `graphics.texture_pack` | folder name under `texture-packs`; not listed in the in-game screen | `--texture-pack` |
| `graphics.output_filter` | `nearest`, `linear`, `fsr` | `--filter` |
| `graphics.antialiasing` | `off`, `smaa` | |
| `graphics.sharpening` | `off`, `low`, `medium`, `high` | |
| `graphics.color_grade` | `neutral`, `vivid`, `warm`, `cool` | |
| `graphics.crt` | `off`, `scanlines`, `crt` | |
| `graphics.scale_mode` | `fit`, `integer`, `stretch` | `--scale` |
| `graphics.window_width`, `graphics.window_height` | pixels | `--window-size WxH` |
| `graphics.fullscreen` | bool | `--fullscreen` |
| `graphics.vsync` | bool | `--no-vsync` |
| `audio.mute` | bool | `--mute` |
| `audio.volume` | 0 to 100 | |
| `audio.device` | device name, empty for the default | |
| `system.memcards` (restart) | bool | `--no-memcard` |
| `system.memcard1`, `system.memcard2` (restart) | paths, empty for the defaults | `--memcard1`, `--memcard2` |
| `system.fast_forward_speed` | `2x`, `4x`, `unlimited` | `--fast-forward-speed` |

## Visual presets

A preset is a named bundle of the graphics options that can change while the game runs.

| | Original | Enhanced | Remaster |
|---|---|---|---|
| internal resolution | 1x | 4x | 8x |
| true colour | off (15-bit, dithered as the game does) | on | on |
| texture filter | nearest | bilinear | xbr |
| precise geometry | off | on | on |
| output filter | nearest | linear | linear |
| anti-aliasing | off | off | off |
| sharpening | off | low | medium |
| colour grading | neutral | neutral | vivid |
| HD backgrounds | off | off | on |

Original is exactly the PlayStation picture. Enhanced makes it sharper without changing the look: higher internal
resolution (this only matters for 3D, because the fields are pre-rendered), no dithering, no vertex wobble, and
bilinear smoothing of 3D textures only; flat sprites and backgrounds are left as drawn. Remaster uses the strongest
settings available, also smooths sprites with xBR, sharpens at the medium level and applies the vivid colour look.
Enhanced adds only the lowest sharpening level, which wins back what the linear scale takes off the 4x picture. SMAA
is off in all three: at 8x the edges are a fraction of a window pixel wide already. Remaster scales linearly instead of
with FSR for the same reason: its picture (2560x1920 and up) is nearly always larger than the window, so there is
nothing to upscale. The CRT look belongs to no preset; it is a separate option that can be switched on in any of them.
Remaster also turns on HD backgrounds; that does nothing until a texture pack is selected (see below).

Changing any of the covered options after picking a preset shows as `custom`; `preset_base` remembers which preset it
started from.

## HD backgrounds (texture packs)

The field backgrounds are pre-rendered pictures that the console stores as small 256-colour images. HD BG replaces
them with sharper versions from a texture pack while everything else (characters, menus, 3D) is drawn as usual.
The game does not come with such a pack: you make one on your own computer from your own disc, using an AI upscaler.
Packs need the GPU renderer; with the software renderer the option is ignored. Best results need an internal
resolution equal to the pack's scale (4x for the default pack); other values work but resample the pack.

Making a pack:

1. Get realesrgan-ncnn-vulkan (the Real-ESRGAN ncnn Vulkan program, MIT licence) and a model for it. The tool works with
   any model in that program's format; `RealESRGAN_General_x4_v3` gave the best results of the ones we tried. Put the
   program and its `models` folder in `<user data dir>/tools`, or give their locations on the command line. The game
   and the pack tool never download anything by themselves.
2. Play the areas you want improved with `--dump-textures <folder>`, for example
   `dw3recomp --dump-textures dump-town`. Every background image that is drawn for a while is recorded; leaving the
   game writes the folder. Use one folder per session; the pack tool takes several. Walk around to scroll through
   larger fields, so that neighbouring images are seen together.
3. Build the pack:
   `texpack generate --dump dump-town --dump dump-inn --out "<user data dir>/texture-packs/general-x4" --name general-x4`
   Options: `--upscaler`, `--models`, `--model` (default `RealESRGAN_General_x4_v3`), `--scale 2|4` (default 4),
   `--gpu N` (if the upscaler picks a software device), `--pad`, `--work`. Running it again with more dumps adds to the
   pack and leaves finished images alone. Each background image is upscaled together with the pixels around it, taken
   from the neighbouring images seen in the dump, so the borders between images line up.
4. Switch HD BG on in the settings screen (or `--hd-backgrounds`, or use the Remaster preset). With
   `graphics.texture_pack` empty the game uses the first folder in `texture-packs` that has a manifest; to pick a
   pack, set `graphics.texture_pack = "general-x4"` in `settings.toml` (or `--texture-pack general-x4`).

Notes and limits:

- An image is replaced only when both the image and the palette it is drawn with match the pack. Palettes that animate
  (water, lights) give one image per palette state that was seen while dumping; states that were never seen show the
  original picture, so let a dump run for a few seconds where something animates.
- Only opaque background images that are drawn one to one are replaced. Characters, objects, menus, 3D models, fades
  and semi-transparent layers are never touched, and what the game reads back from the screen is unchanged.
- Loading a save state restores images the game uploaded earlier. They are picked up when first drawn, but only as the
  rectangles that were drawn, so a pack made from a normal session fits best.
- A pack contains pictures from your disc: do not share it.

## Post-processing

These options act on the finished picture (the part of VRAM the console would display), after the internal-resolution
rendering and before or while it is scaled to the window. They need the GPU renderer: the software path ignores them
and the settings screen still lists them. With all of them off the output is bit-for-bit what it is without the
feature. The order of the passes is described in [ARCHITECTURE.md](ARCHITECTURE.md#post-processing).

- **Anti-aliasing** `smaa`: SMAA 1x. Smooths stair-stepped edges; mostly useful at internal resolution 1x to 2x.
- **Sharpening**: contrast adaptive sharpening (CAS) at three strengths. Flat areas are never changed. With the `fsr`
  output filter this setting drives FSR's own sharpening step instead (off skips it).
- **Output filter** `fsr`: FidelityFX Super Resolution 1 (edge-adaptive upscale plus sharpening). It improves on
  `linear` when the window is larger than the picture; if the picture is not enlarged on either axis (an 8x picture
  in a small window), it falls back to `linear`.
- **Colour grading**: `neutral` leaves colours alone; `vivid` adds saturation, contrast and a little vibrance; `warm`
  and `cool` tint the picture.
- **CRT look**: `scanlines` darkens the gaps between the console's lines (beams widen on bright colours);
  `crt` adds a phosphor mask, slight screen curvature with rounded corners and a vignette. Scanlines follow the
  console's lines, not the window's, so they look best at windows several times the console resolution.

## Hotkeys

| Key | Action |
|---|---|
| F9 | Switch between Original and the preset in `graphics.comparison_preset`, for comparing side by side. Not saved. Pressing it again restores your exact previous settings. |
| Tab (hold) | Fast-forward while held |
| F10 | Latch fast-forward on or off |
| F11, Alt+Enter | Fullscreen |
| F5 / F7, F6 / F8 | Save state / load state, previous / next slot |

On a gamepad, pressing the right stick (R3) fast-forwards while held and pressing the left stick (L3) latches it;
the game does not use either.

## Settings screen in the game

With the START menu open in the field, SELECT turns its list into a settings list in the game's own windows (the
header shows the hint). Up and down move the cursor, left and right change the value, and circle or triangle returns to
the menu. Every change applies at once and is written to `settings.toml` (nothing is written with `--no-config`).

It lists the options that take effect while playing: the visual preset first, then the graphics options (3D scale,
true colour, texture filter, precise geometry, HD BG, output filter, scaling, window size, fullscreen, vsync), mute and
volume, and the fast-forward speed. The options that need a restart (renderer, memory cards) stay in the file. Five
rows show at a time (six when the menu has CARD FOLDERS); the list scrolls.

## Fast-forward

`system.fast_forward_speed` sets the speed for both the hold key and the latch: 2x, 4x or unlimited. While it is on,
the picture is presented without waiting for the display and the sound is muted; normal speed resumes from an empty
audio queue, so there is no crackling or added delay.
