# macOS packaging

Builds `Digimon World 3 Recomp.app` (bundle id `io.github.mikeravenelle.dw3recomp`) and a disk image. Needs the Xcode
command line tools (`swift`, `iconutil`, `codesign`, `hdiutil`, `notarytool`).

```sh
cmake --preset macos-arm64-release -DDW3_GAME_DIR=$HOME/dw3/game -DDW3_FETCH_SDL=ON
cmake --build --preset macos-arm64-release
packaging/macos/make_app.sh build/macos-arm64-release         # -> build/macos-arm64-release/macos-app/*.app, signed ad hoc
DW3_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" packaging/macos/sign.sh "<app>"
DW3_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" packaging/macos/make_dmg.sh "<app>" DW3Recomp.dmg
DW3_NOTARY_PROFILE=<keychain profile> packaging/macos/notarize.sh DW3Recomp.dmg
```

- `DW3_FETCH_SDL=ON` links SDL statically, so the app has no library of a package manager inside it and its deployment
  target (13.0, set in the presets and written to `LSMinimumSystemVersion`) is true. `make_app.sh` still copies any other
  non-system dylib into `Contents/Frameworks` and points the executable at it (`@executable_path/../Frameworks`).
- `make_app.sh` signs ad hoc, which is enough to run the app on the machine that built it. Unsigned or ad-hoc builds that
  were downloaded need right-click > Open (Gatekeeper).
- The icon is a generated placeholder (`make_icon.swift`: "DW3" on a blue square); put real art in as an `.icns` and
  copy it instead.
- User data (settings, memory cards, save states, the HD tools and packs) lives in
  `~/Library/Application Support/DW3Recomp`, not in the bundle, so the app can sit on a read-only image.
- `sign.sh` uses the hardened runtime (`--options runtime`) and needs no entitlements: Metal, SDL and the game logic
  use no JIT, no unsigned memory, no camera, microphone or network server, and the app is not sandboxed. The only
  case that needs one is signing ad hoc with a bundled dylib (library validation compares Team IDs, and ad hoc has
  none); `entitlements-adhoc.plist` (`com.apple.security.cs.disable-library-validation`) is for that test and must
  not be used for a release (pass it in `DW3_ENTITLEMENTS`).
- The HD backgrounds setup downloads the upscaler with `curl` and unpacks it itself, so the files carry no
  `com.apple.quarantine` attribute (only `com.apple.provenance`, which does not block execution) and the hardened
  runtime does not restrict starting that separate process. Both were tested on macOS 27.
- Game controllers need no `Info.plist` key (GameController and IOKit, no Bluetooth API). The folder usage strings
  are shown if the disc image is in Documents, Downloads, Desktop or a removable volume.
