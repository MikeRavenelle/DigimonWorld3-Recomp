# Digimon World 3 Recomp

A static recompilation of *Digimon World 3* (PlayStation, 2002, NTSC-U SLUS-01436) into a native PC
executable.

The game's MIPS code (the main executable and all 263 overlays on the disc) is translated ahead of time
into C and compiled for the host, then linked against a runtime that reimplements the PlayStation
hardware and kernel. There's no emulated CPU and no interpreter. The goal is a native port that plays the
same as the original and leaves room for things an emulator can't do as cleanly: higher resolutions,
widescreen, quality-of-life options and modding.

## Status

Early development. Nothing is released yet, and `main` only gets code once there is a build worth
releasing. Work happens on feature branches.

At the moment the recompiled game boots, plays the intro movies, reaches the title screen and menus, and
starts a new game in Kusanagi City. Memory card saves are in progress, and audio is being worked on.

Planned targets:

- Linux x86_64 (primary)
- macOS (Apple Silicon)
- Linux ARM64 and Windows later

Rendering will go through Vulkan, Metal and Direct3D 12.

## Game data

This repository contains no game data. You need your own copy of the game, dumped as a bin/cue image.
Nothing derived from the game (extracted files, recompiled code, screenshots) is ever committed.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Bandai Namco or Sony. Digimon is a
trademark of its respective owners.

## License

GPL-3.0. See [LICENSE](LICENSE).
