#!/bin/bash
# Signs the app with the hardened runtime. Usage: sign.sh <app> ; DW3_SIGN_IDENTITY is the signing identity, for example
# "Developer ID Application: Name (TEAMID)"; "-" signs ad hoc (to test the hardened runtime without a certificate).
# DW3_ENTITLEMENTS names an entitlements file; none is needed today (see README.md).
set -euo pipefail

app="${1:?usage: sign.sh <app>}"
identity="${DW3_SIGN_IDENTITY:?set DW3_SIGN_IDENTITY to the signing identity}"
options=(--force --options runtime --sign "$identity")
[ "$identity" = "-" ] || options+=(--timestamp)
[ -z "${DW3_ENTITLEMENTS:-}" ] || options+=(--entitlements "$DW3_ENTITLEMENTS")

# Inside out: libraries first, then the bundle itself.
[ ! -d "$app/Contents/Frameworks" ] || find "$app/Contents/Frameworks" -type f -name '*.dylib' -exec codesign "${options[@]}" {} +
codesign "${options[@]}" "$app"
codesign --verify --deep --strict --verbose=2 "$app"
