#!/bin/bash
# Makes a disk image with the app and an Applications shortcut. Usage: make_dmg.sh <app> <output.dmg>
set -euo pipefail

app="${1:?usage: make_dmg.sh <app> <output.dmg>}"
dmg="${2:?usage: make_dmg.sh <app> <output.dmg>}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
rm -f "$dmg"
hdiutil create -volname "Digimon World 3 Recomp" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg"
# A signed app gets a signed image too, so Gatekeeper can check the image before it is opened.
if [ -n "${DW3_SIGN_IDENTITY:-}" ] && [ "$DW3_SIGN_IDENTITY" != "-" ]; then
    codesign --force --timestamp --sign "$DW3_SIGN_IDENTITY" "$dmg"
fi
echo "$dmg"
