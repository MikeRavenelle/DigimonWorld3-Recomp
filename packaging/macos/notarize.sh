#!/bin/bash
# Notarizes a signed disk image (or zip) and staples the ticket. Usage: notarize.sh <file.dmg>
# DW3_NOTARY_PROFILE names a keychain profile created once with `xcrun notarytool store-credentials`.
set -euo pipefail

file="${1:?usage: notarize.sh <file.dmg>}"
profile="${DW3_NOTARY_PROFILE:?set DW3_NOTARY_PROFILE to the notarytool keychain profile}"

xcrun notarytool submit "$file" --keychain-profile "$profile" --wait
xcrun stapler staple "$file"
xcrun stapler validate "$file"
