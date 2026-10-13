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

Keys are grouped in `[graphics]`, `[audio]`, `[system]` and `[gameplay]`. Settings marked "restart" take effect on the next start;
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
| `graphics.output_filter` | `nearest`, `linear` | `--filter` |
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
| `gameplay.encounters` | `normal`, `half`, `off`, `double` | `--encounters` |
| `gameplay.game_speed` | `1x`, `2x`, `3x`, `4x` | `--game-speed` |
| `gameplay.god_mode` | bool | `--god-mode` |

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

## Visual presets

A preset is a named bundle of the graphics options that can change while the game runs.

| | Original | Enhanced | Remaster |
|---|---|---|---|
| internal resolution | 1x | 4x | 8x |
| true colour | off (15-bit, dithered as the game does) | on | on |
| texture filter | nearest | bilinear | xbr |
| precise geometry | off | on | on |
| output filter | nearest | linear | linear |

Original is exactly the PlayStation picture. Enhanced makes it sharper without changing the look: higher internal
resolution (this only matters for 3D, because the fields are pre-rendered), no dithering, no vertex wobble, and
bilinear smoothing of 3D textures only; flat sprites and backgrounds are left as drawn. Remaster uses the strongest
settings available and also smooths sprites with xBR. Later versions will add post-processing and replacement
textures to it.

Changing any of the covered options after picking a preset shows as `custom`; `preset_base` remembers which preset it
started from.

## Hotkeys

| Key | Action |
|---|---|
| F9 | Switch between Original and the preset in `graphics.comparison_preset`, for comparing side by side. Not saved. Pressing it again restores your exact previous settings. |
| Tab (hold) | Fast-forward while held |
| F10 | Latch fast-forward on or off |
| F4 | Cycle game speed 1x, 2x, 3x, 4x |
| F11, Alt+Enter | Fullscreen |
| F5 / F7, F6 / F8 | Save state / load state, previous / next slot |

On a gamepad, pressing the right stick (R3) fast-forwards while held and pressing the left stick (L3) latches it;
the game does not use either.

## Settings screen in the game

With the START menu open in the field, SELECT turns its list into a settings list in the game's own windows (the
header shows the hint). Up and down move the cursor, left and right change the value, and circle or triangle returns to
the menu. Every change applies at once and is written to `settings.toml` (nothing is written with `--no-config`).

It lists the options that take effect while playing: the visual preset first, then the graphics options (3D scale,
true colour, texture filter, precise geometry, output filter, scaling, window size, fullscreen, vsync), mute and
volume, and the fast-forward speed. The options that need a restart (renderer, memory cards) stay in the file. Five
rows show at a time (six when the menu has CARD FOLDERS); the list scrolls.

## Fast-forward

`system.fast_forward_speed` sets the speed for both the hold key and the latch: 2x, 4x or unlimited. While it is on,
the picture is presented without waiting for the display and the sound is muted; normal speed resumes from an empty
audio queue, so there is no crackling or added delay.
