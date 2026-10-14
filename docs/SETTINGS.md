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

Keys are grouped in `[graphics]`, `[audio]`, `[controls]`, `[system]` and `[gameplay]`. Settings marked "restart" take effect on the next start;
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
| `graphics.seamless_bg` | bool | `--seamless-bg` |
| `graphics.texture_pack` | folder name under `texture-packs`; not listed in the in-game screen | `--texture-pack` |
| `graphics.output_filter` | `nearest`, `linear`, `fsr` | `--filter` |
| `graphics.antialiasing` | `off`, `smaa` | |
| `graphics.sharpening` | `off`, `low`, `medium`, `high` | |
| `graphics.color_grade` | `neutral`, `vivid`, `warm`, `cool` | |
| `graphics.crt` | `off`, `scanlines`, `crt` | |
| `graphics.scale_mode` | `fit`, `integer`, `stretch` | `--scale` |
| `graphics.widescreen` | `off`, `16:9`, `16:10`, `21:9` | `--widescreen` |
| `graphics.display_mode` | `windowed`, `borderless`, `exclusive` | `--display-mode`, `--fullscreen` (borderless) |
| `graphics.monitor` | 0 (auto: where the window is), 1 and up | `--monitor` |
| `graphics.window_width`, `graphics.window_height` | pixels; not listed in the in-game screen, which offers common sizes | `--window-size WxH` |
| `graphics.fullscreen_width`, `graphics.fullscreen_height`, `graphics.fullscreen_refresh` | exclusive mode; refresh in hundredths of a hertz; 0 for the desktop mode or the highest refresh rate; not listed | |
| `graphics.vsync` | `off`, `on`, `adaptive` | `--no-vsync` |
| `graphics.frame_pacing` | `off`, `even` | |
| `graphics.show_fps` | bool | `--show-fps` |
| `graphics.gamma` | 80 to 140 (percent) | `--gamma` |
| `graphics.brightness` | -50 to 50 (percent) | `--brightness` |
| `graphics.hdr` | bool | |
| `graphics.hdr_peak_nits` | 400 to 1500 (steps of 50 in the in-game screen) | |
| `graphics.hdr_paper_white` | 100 to 300 (steps of 10 in the in-game screen) | |
| `audio.mute` | bool | `--mute` |
| `audio.volume` | 0 to 100 | |
| `audio.music_volume` | 0 to 100 (percent; steps of 10 in the in-game screen) | |
| `audio.sfx_volume` | 0 to 100 (percent; steps of 10 in the in-game screen) | |
| `audio.device` | device name, empty for the default | |
| `controls.pad` | `auto`, `1` to `4` | |
| `controls.left_stick` | bool | |
| `controls.deadzone` | 10 to 50 (percent) | |
| `controls.prompts` | `auto`, `xbox`, `playstation`, `nintendo` | |
| `controls.<action>` | binding text, see Controls | |
| `system.memcards` (restart) | bool | `--no-memcard` |
| `system.memcard1`, `system.memcard2` (restart) | paths, empty for the defaults | `--memcard1`, `--memcard2` |
| `system.disc_path` (restart) | path of the disc image (`.cue`); written by the first-run setup, not listed in the in-game screen | `--disc` |
| `system.fast_forward_speed` | `2x`, `4x`, `unlimited` | `--fast-forward-speed` |
| `gameplay.encounters` | `normal`, `half`, `off`, `double` | `--encounters` |
| `gameplay.game_speed` | `1x`, `2x`, `3x`, `4x` | `--game-speed` |
| `gameplay.god_mode` | bool | `--god-mode` |
| `gameplay.xp_multiplier` | `1x`, `2x`, `3x`, `5x`, `10x` | `--xp-multiplier` |
| `gameplay.text_speed` | `normal`, `fast`, `instant` | `--text-speed` |

## Gameplay options

All of these default to the original game and apply immediately; they also appear in the in-game settings screen.

- **Random encounters** changes how often walking starts a battle: `half` and `double` change the rate of the game's
  own step countdown, `off` never starts one. Scripted battles are not affected.
- **Game speed** runs the whole game (field, menus, battles, cutscenes) at 2x to 4x. Sound keeps playing at its normal
  pitch: the SPU output is time-compressed with a waveform-similarity overlap-add stretcher, so music and effects stay
  in tune but cover more game time per second. F4 (the gamepad's Guide button) cycles the speed for this session.
  The fast-forward keys below still override it while active (and, unlike game speed, silence the sound).
- **God mode** makes the party take no damage and defeat enemies with the first hit that lands, in every battle. Items
  and status effects are untouched. It is off by default and not meant for a first playthrough.
- **XP multiplier** (1x, 2x, 3x, 5x, 10x) scales the experience each Digimon receives when a won battle's results are
  applied; the results screen lists the scaled award, and the total stays capped at the game's 999,999.
- **Text speed** sets how quickly message boxes print (dialogue, menu descriptions, battle text). `fast` prints three
  characters per frame instead of one every few frames; `instant` prints each box at once and still waits for the
  button to close it. Pauses written into the text and scripted timing are unchanged.

## Music and effects volume

`audio.music_volume` and `audio.sfx_volume` scale the two kinds of sound the game plays, below the master `audio.volume`.
The game's sound driver records, for each voice it keys on, the sequence that started it. Music plays from sequence
slots 2 and 3, which the field, the battle and the card game load alternately; the effect banks are slots 0 and 1, and
effects started directly use a fixed id. The SPU mixes each group at its own level before reverb. Movie and other CD
audio follows the music level. The victory fanfare comes from an effect bank, so it follows the effects level.

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

The first-run setup (`dw3recomp --setup`) can do all of this for you: on "Accept" it downloads the upscaler and the model
into `<user data dir>/tools` (checked by SHA-256, with the system `curl`), builds a pack named `disc-hd` from the disc,
and sets `graphics.hd_backgrounds` and `graphics.texture_pack`. The steps below are the manual route.

Seamless backgrounds (`graphics.seamless_bg`, "Seamless BG" in the settings screen, GPU renderer only) remove the blocks
that pop in at the edges of a field map while it scrolls. The game keeps only a dozen map cells in memory and covers the
rest with dark squares; with the option on, the missing pieces of the map are drawn from the disc's own map files and the
squares are hidden. It works with and without an HD pack (the pack's images are used where it has them), at any internal
resolution, and is not part of any preset. The map files are indexed in the background when the option is first turned
on, so it takes effect a few seconds later. Limits: while a map is still loading, before the game has drawn any of its
cells, the game's squares remain; a foreground piece is only drawn in frames where the game draws some foreground piece.

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

To cover every field map without playing, give `texpack` the disc instead of dumps:
`texpack generate --disc "<disc .cue or .bin, or an extracted copy>" --out "<user data dir>/texture-packs/general-x4"
--name general-x4` reads the background images from the disc's stage pack files and upscales them with the same options as
above (`--disc` can be combined with `--dump`). It takes a long time. `texpack extract --disc ... --out dump-all`
only writes the dump, and `--map S226` limits either command to one map.

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

## Controls

- **Gamepad hotplug**: pads can be plugged in and removed while playing. With `controls.pad = auto` the pad in use is
  kept until it is removed, then the next connected one takes over. `1` to `4` pick a pad by its position among the
  connected ones and fall back to auto when that pad is not connected.
- **Left stick** moves like the D-pad (8 directions) once it leans past `controls.deadzone`, a round dead zone given as
  a percentage of the stick's travel. Triggers are L2 and R2. The game reads a digital pad only, so the sticks carry
  no analog information and the right stick is free (pressing it is the fast-forward hold).
- **Vibration**: DW3 sets up the DualShock motors at start but never commands them, so there is no rumble setting.
- **Bindings** are `[controls]` keys named after the action (`up`, `down`, `left`, `right`, `cross`, `circle`,
  `square`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `start`, `select`, `fast_forward`, `fast_forward_latch`,
  `game_speed`, `compare`), each `"<key>|<button>"`: for example `cross = "Z|South"`. Key names are SDL's
  (`Return`, `Left Shift`, `F9`); buttons are `South`, `East`, `West`, `North`, `Back`, `Guide`, `Start`, `LeftStick`,
  `RightStick`, `LeftShoulder`, `RightShoulder`, `DPadUp`, `DPadDown`, `DPadLeft`, `DPadRight`, `LeftTrigger`,
  `RightTrigger`, named by position. Either half may be empty. Assigning a key or button takes it from the action that
  had it. The arrow keys, Return, Backspace, Space and Escape always act as D-pad, Start, Select, Cross and Circle
  and cannot be bound to another action, so the menu stays reachable whatever is bound.
- **Button prompts** (`controls.prompts`) choose the names shown for gamepad buttons in hints: `auto` follows the
  connected pad (Xbox names when there is none). South stays the console's Cross on every pad; only the names change.

## Hotkeys

These are the defaults; F9, Tab, F10 and F4 can be rebound (see Controls).

| Key | Action |
|---|---|
| F9 | Switch between Original and the preset in `graphics.comparison_preset`, for comparing side by side. Not saved. Pressing it again restores your exact previous settings. |
| Tab (hold) | Fast-forward while held |
| F10 | Latch fast-forward on or off |
| F4 | Cycle game speed 1x, 2x, 3x, 4x |
| F11, Alt+Enter | Fullscreen |
| F5 / F7, F6 / F8 | Save state / load state, previous / next slot |

On a gamepad, pressing the right stick fast-forwards while held and pressing the left stick latches it; the Guide
button cycles the game speed.

## Settings screen in the game

With the START menu open in the field, SELECT turns its list into a settings list in the game's own windows (the
header shows the hint, naming the gamepad's own button when a pad is connected). Up and down move the cursor, left and
right change the value, L1 and R1 jump between the pages (Graphics, Audio, Controls, Gameplay; the header names the
page and the row's position in it), and circle or triangle returns to the menu. Every change applies at once and is written to `settings.toml` (nothing is written with `--no-config`).

It lists the options that take effect while playing: the visual preset first, then the graphics options (3D scale,
true colour, texture filter, precise geometry, HD BG, output filter, scaling, display mode, monitor, resolution, vsync,
frame pacing, FPS counter, gamma, brightness), mute and
volume, the controls (gamepad, left stick, deadzone, button prompts, then one row per binding such as `Cross: Z/A`,
and a Reset Controls row) and the gameplay options. Cross on a binding row waits for the next key or gamepad button;
Escape or five seconds cancel. The options that need a restart (renderer, memory cards) stay in the file. Five
rows show at a time (six when the menu has CARD FOLDERS); the list scrolls.

The same screen opens from the title menu: while Start / Continue waits for input, SELECT (the hint is shown at the top)
opens the list over the dimmed title. The menu stands still until circle or triangle closes it, and then waits as
before. Options that only matter in the field, such as the encounter rate, can be set there and apply once playing.

## HDR output

- Needs the GPU renderer on Direct3D 12, Vulkan or Metal, with the display in HDR mode (Windows: Settings > Display
  > HDR; Linux: a compositor with HDR support, for example KDE Plasma 6.x on Wayland; macOS: an XDR or other EDR
  display, nothing to switch on). The in-game rows read `N/A` until the display reports HDR.
- On macOS the picture is placed relative to the system's SDR white (the brightness slider), so the paper white
  setting only matters for the ratio of peak to paper white; the peak is also limited to what the display grants.
- The picture is converted to scRGB (linear Rec. 709 half floats): SDR white is placed at the paper white level
  (`graphics.hdr_paper_white`, default 203 nits) and only the brightest part is lifted, smoothly, towards the peak
  (`graphics.hdr_peak_nits`, default 1000). Colours keep their hue and saturation; there is no gamut expansion.
- If the driver or compositor does not offer an HDR swap chain the picture stays SDR (the log says so). Screenshots
  of the window (`--screenshot`) are not taken while HDR output is on.
- Not verified on real HDR hardware by the author: the maths and the shader are covered by tests against the CPU
  formulas, the swap chain selection is not.

## Widescreen

- Needs the GPU renderer. While a battle or a field runs, the picture is widened to the chosen aspect (16:9, 16:10 or
  21:9) and the world continues past the sides of the original 4:3 view. Menus, status windows and message boxes keep
  their original size and place in the centre; flat shapes that touch the frame's edge (fades, backdrops) stretch to
  the new edges. Everything else (scenes, the results screen, menus) stays 4:3 with borders.
- The GPU renderer keeps a wider copy of the battle's frame buffers in extra render-target columns and draws each
  battle primitive into it a second time, so the game itself is not changed. The extra columns cost memory only
  while this setting is on: at internal resolution 8x and 21:9 the two VRAM targets grow from about 268 MB to
  about 415 MB.
- Anything the game itself skips outside its 4:3 view cannot be shown; the arena checked so far submits its ground
  and sky well past the view.
- Fields: the margins are filled with the pieces of the map's pre-rendered background (read from the stage packs on
  your disc, or from the HD pack where it has them), whether or not `graphics.seamless_bg` is on; that option only
  decides whether the 4:3 area is completed as well. Characters and objects need nothing special: the field submits
  every active actor to the GPU every frame whatever its position, so those that stand in a margin are drawn. Beyond
  the edge of a map the margin is black, as the game paints the void. A field frame whose map cannot be worked out
  (cutscenes, transitions, a map without a stage pack) is shown at 4:3 with borders, so a margin never shows
  leftovers.

## Display

- Display mode: `windowed`, `borderless` (a desktop-sized window without borders) or `exclusive` (the monitor
  switches to the chosen mode). F11 and Alt+Enter toggle between windowed and the last fullscreen mode. A
  settings.toml with the old `fullscreen = true` loads as `borderless`, and `vsync = false` or `true` as `off` or `on`.
- Monitor: auto follows the window; the in-game row shows a monitor's name when it is short, otherwise its number.
- Resolution (in-game row): windowed offers common 4:3, 16:9 and 16:10 sizes that fit the monitor's desktop area;
  borderless uses the desktop; exclusive lists the monitor's own modes with their refresh rates.
- Vertical sync `adaptive` needs a presenter that supports it (the software path through SDL, where the driver
  allows); the GPU renderer treats it as `on`, and the in-game row does not offer it there.
- Frame pacing `even` (with vertical sync on) shows each emulated frame for the same whole number of refreshes when the
  display really delivers 120, 180 or 240 Hz. The refresh rate the system reports is only a hint: the game measures the
  time between back-to-back presents (a rolling median) and repeats presents only while that interval is about 1/120,
  1/180 or 1/240 s. It checks again every few seconds and at once drops back to a single present when the interval is
  longer (a compositor frame limiter such as the one in Steam's Game Mode, a variable refresh rate display) or when
  emulation would fall behind real time, so the game always runs at 59.94 Hz. Other rates, such as 144 Hz, keep the
  plain behaviour. There is no frame interpolation.
- Show FPS draws the emulated frame rate small in the top-right corner (not part of screenshots).
- Gamma and brightness are applied in the colour-grading pass of the GPU renderer, after the colour grade. They are
  not part of any visual preset, like the display options.

## Fast-forward

`system.fast_forward_speed` sets the speed for both the hold key and the latch: 2x, 4x or unlimited. While it is on,
the picture is presented without waiting for the display and the sound is muted; normal speed resumes from an empty
audio queue, so there is no crackling or added delay.
