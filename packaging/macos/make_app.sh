#!/bin/bash
# Builds "Digimon World 3 Recomp.app" from a finished build. Usage: make_app.sh <build dir> [output dir]
# Environment: DW3_VERSION (default: the project version), DW3_MIN_OS (default 13.0, the deployment target of the presets).
# The result is signed ad hoc (arm64 binaries must carry a signature to run); sign.sh signs it for distribution.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$(cd "${1:?usage: make_app.sh <build dir> [output dir]}" && pwd)"
out="${2:-$build/macos-app}"
version="${DW3_VERSION:-$(sed -n 's/^project(DW3Recomp VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")}"
min_os="${DW3_MIN_OS:-13.0}"
app="$out/Digimon World 3 Recomp.app"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp "$build/app/dw3recomp" "$app/Contents/MacOS/dw3recomp"
cp "$root/LICENSE" "$app/Contents/Resources/LICENSE"
sed -e "s/@VERSION@/$version/g" -e "s/@MIN_OS@/$min_os/g" "$here/Info.plist.in" > "$app/Contents/Info.plist"

cp "$root/packaging/icons/dw3recomp.icns" "$app/Contents/Resources/AppIcon.icns"

# Bundle every non-system dylib the executable needs (and theirs), pointing their install names at the bundle.
exe="$app/Contents/MacOS/dw3recomp"
bundle_dylibs() {
    local target="$1" dep name
    while IFS= read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) continue ;;
        esac
        name="$(basename "$dep")"
        if [ ! -f "$app/Contents/Frameworks/$name" ]; then
            cp "$dep" "$app/Contents/Frameworks/$name"
            chmod u+w "$app/Contents/Frameworks/$name"
            install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
            bundle_dylibs "$app/Contents/Frameworks/$name"
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$target"
    done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
}
bundle_dylibs "$exe"
if [ -z "$(ls -A "$app/Contents/Frameworks")" ]; then
    rmdir "$app/Contents/Frameworks"  # SDL is linked statically in the release build: nothing to bundle
elif ! otool -l "$exe" | grep -q "@executable_path/../Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe"
fi

# Sign every Mach-O inside out, ad hoc.
[ ! -d "$app/Contents/Frameworks" ] || find "$app/Contents/Frameworks" -type f -name '*.dylib' -exec codesign --force --sign - {} +
codesign --force --sign - "$app"
echo "$app"
