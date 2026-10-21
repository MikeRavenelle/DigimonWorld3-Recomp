Digimon World 3 Recomp (Windows)

This is a native port of Digimon World 3 (USA, PlayStation). It does not contain the game: you need your own
disc image of the USA release (Digimon World 3 (USA).cue and its .bin file).

Run
---
    dw3recomp.exe

On first start a setup window asks for your disc image (.cue), saves its location and builds the game from it on this
computer (a few minutes, once; nothing is downloaded for this). Run
"dw3recomp.exe --setup" to open it again. To use a disc for one run only:
    dw3recomp.exe --disc "C:\Games\Digimon World 3 (USA).cue"

The path may contain spaces and non-ASCII characters.

Memory cards, save states and the disc index cache are kept in %APPDATA%\DW3Recomp.

Keys
----
    Arrow keys     D-pad              Z / X / A / S   Cross / Circle / Square / Triangle
    Enter          Start              Backspace       Select
    Q / W          L1 / R1            1 / 2           L2 / R2
    F5             save state         F7              load state
    F6 / F8        previous / next state slot (1 to 9)
    F9             Original <-> enhanced picture (comparison)
    Tab (hold)     fast-forward       F10             latch fast-forward
    F11, Alt+Enter fullscreen         Close window  quit
A gamepad that SDL recognizes is used as well.

Options
-------
    --renderer auto|gpu|d3d12|vulkan|software    graphics API (default: Direct3D 12, then Vulkan, then software)
    --window-size 1280x960   --fullscreen        window
    --internal-res N                             GPU renderer internal resolution multiplier, 1 to 8
    --mute                                       no audio
    --preset original|enhanced|remaster          visual preset (default: original)
    --config PATH   --no-config                  settings file to use, or none

Settings are kept in %APPDATA%\DW3Recomp\settings.toml (created on the first start, commented). Command-line
options override it for one run and are not saved. See docs/SETTINGS.md for every option.

License: GPL-3.0 (see LICENSE).
