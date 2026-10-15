# Icons

One design, drawn by `make_icons.swift` (a digital egg dissolving into pixels, original artwork), used on every platform:

| File | Used for |
|---|---|
| `dw3recomp.icns` | macOS: `packaging/macos/make_app.sh` copies it into the `.app` |
| `dw3recomp.ico` | Windows: linked into `dw3recomp.exe` by `app/windows/dw3recomp.rc` |
| `png/dw3recomp-<size>.png` | Linux: installed to `share/icons/hicolor/<size>x<size>/apps/` with `packaging/linux/dw3recomp.desktop` |
| `window-icon-128.rgba` | all: embedded in the executable and set on every window with `SDL_SetWindowIcon` (`app/icon`) |
| `dw3recomp-1024.png` | master image for other uses |

The generated files are committed, so building needs no extra tools. To change the design, edit `make_icons.swift` and run
`packaging/icons/make_icons.sh` on a Mac (needs `swift`, `iconutil` and `python3`).
