#!/usr/bin/env bash
# Runs a Windows executable built by the windows-x64-cross presets under Wine, with vkd3d-proton providing Direct3D 12.
#
#   tools/wine/run-wine.sh build/windows-x64-cross-release/tests/test_render.exe [args...]
#
# It uses the Wine and vkd3d-proton of a Proton build (Proton-GE or Steam's Proton), which ships both. Environment:
#   DW3_PROTON      directory of the Proton build (the one containing files/bin/wine); searched for in the Steam
#                   folders when unset
#   DW3_WINEPREFIX  Wine prefix to use; created on first use
#                   (default: ${XDG_DATA_HOME:-~/.local/share}/dw3-wine/pfx)
#   DW3_WINEDEBUG   value for WINEDEBUG (default: -all)
set -euo pipefail

proton="${DW3_PROTON:-}"
if [[ -z "$proton" ]]; then
    for dir in "$HOME"/.steam/steam/compatibilitytools.d/*/ "$HOME"/.steam/steam/steamapps/common/Proton*/; do
        if [[ -x "${dir}files/bin/wine" ]]; then
            proton="${dir%/}"
        fi
    done
fi
if [[ -z "$proton" || ! -x "$proton/files/bin/wine" ]]; then
    echo "run-wine.sh: set DW3_PROTON to a Proton build directory (containing files/bin/wine)" >&2
    exit 1
fi

prefix="${DW3_WINEPREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/dw3-wine/pfx}"
export PATH="$proton/files/bin:$PATH"
export LD_LIBRARY_PATH="$proton/files/lib/x86_64-linux-gnu:$proton/files/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -d "$prefix" ]]; then
    mkdir -p "$prefix"
    WINEPREFIX="$prefix" WINEDEBUG=-all "$proton/files/bin/wine" wineboot --init >/dev/null 2>&1 || true
    WINEPREFIX="$prefix" "$proton/files/bin/wineserver" -w || true
    # vkd3d-proton replaces Wine's own Direct3D 12 implementation, and DXVK's DXGI goes with it.
    for dll_dir in "$proton/files/lib/wine/vkd3d-proton/x86_64-windows" "$proton/files/lib/wine/dxvk/x86_64-windows"; do
        if [[ -d "$dll_dir" ]]; then
            cp -f "$dll_dir"/*.dll "$prefix/drive_c/windows/system32/"
        fi
    done
fi

export WINEPREFIX="$prefix"
export VKD3D_SHADER_CACHE_PATH="$prefix/vkd3d-cache"
export WINEDEBUG="${DW3_WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-};d3d12,d3d12core,dxgi,d3d11,d3d10core,d3d9=n,b"
exec "$proton/files/bin/wine" "$@"
