#!/bin/bash
# Regenerates the icon files from make_icons.swift (needs macOS: swift, iconutil, python3). The results are committed:
#   dw3recomp.icns   macOS app icon
#   dw3recomp.ico    Windows executable icon (16 to 256 px)
#   png/             Linux hicolor icons (16 to 512 px), installed by the Linux package
#   window-icon-128.rgba   raw RGBA window icon embedded in the executable (SDL window icon on Linux and Windows)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

swift "$here/make_icons.swift" "$work"

rm -rf "$here/png"; mkdir -p "$here/png"
for px in 16 32 48 64 128 256 512; do cp "$work/icon-$px.png" "$here/png/dw3recomp-$px.png"; done
cp "$work/icon-1024.png" "$here/dw3recomp-1024.png"
cp "$work/icon-128.rgba" "$here/window-icon-128.rgba"

iconset="$work/AppIcon.iconset"; mkdir -p "$iconset"
cp "$work/icon-16.png"   "$iconset/icon_16x16.png";      cp "$work/icon-32.png"   "$iconset/icon_16x16@2x.png"
cp "$work/icon-32.png"   "$iconset/icon_32x32.png";      cp "$work/icon-64.png"   "$iconset/icon_32x32@2x.png"
cp "$work/icon-128.png"  "$iconset/icon_128x128.png";    cp "$work/icon-256.png"  "$iconset/icon_128x128@2x.png"
cp "$work/icon-256.png"  "$iconset/icon_256x256.png";    cp "$work/icon-512.png"  "$iconset/icon_256x256@2x.png"
cp "$work/icon-512.png"  "$iconset/icon_512x512.png";    cp "$work/icon-1024.png" "$iconset/icon_512x512@2x.png"
iconutil -c icns "$iconset" -o "$here/dw3recomp.icns"

# .ico with PNG-compressed images (Windows Vista and later); 256 is stored as 0 in the directory.
python3 -I - "$work" "$here/dw3recomp.ico" <<'PY'
import struct, sys
work, out = sys.argv[1:]
sizes = [16, 24, 32, 48, 64, 128, 256]
images = [open(f"{work}/icon-{s}.png", "rb").read() for s in sizes]
offset = 6 + 16 * len(sizes)
data = struct.pack("<HHH", 0, 1, len(sizes))
for s, img in zip(sizes, images):
    data += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
    offset += len(img)
open(out, "wb").write(data + b"".join(images))
PY
echo "icons written to $here"
