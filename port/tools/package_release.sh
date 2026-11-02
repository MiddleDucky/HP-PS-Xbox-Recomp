#!/usr/bin/env bash
# The build players get: configures build-release (HPPS_RELEASE=ON: buffer-overflow hardening on the hand-written
# code, SSE4.1, no debug keys), builds the game and the launcher, and puts in dist/hpps-<date>/:
#   hpps_launcher.exe             setup: 4:3 / 16:9, graphics, controls; starts the game
#   hpps.exe                      the game
#   shadercache\base_shaders.bin  shaders compiled ahead (read only; the player's own go in shaders.bin)
#   README.txt                    requirements, installing, controls, the F10 options
# Players put the folder beside (or the exes inside) their game/ folder (default.xbe, data); hpps.toml, saves\
# and the rest of shadercache\ are created on first run. Symbols stay in hpps.exe so crash reports name guest functions.
set -euo pipefail
port="$(cd "$(dirname "$0")/.." && pwd)"
cd "$port"
# native on Windows (MSYS2 UCRT64 shell: gcc, objdump, nm) since 2026-10-20; cross from Linux with the toolchain file
if command -v x86_64-w64-mingw32-objdump > /dev/null; then
    cross=(-DCMAKE_TOOLCHAIN_FILE="$port/../cmake/mingw-w64-x86_64.cmake")
    objdump=x86_64-w64-mingw32-objdump
    nm=x86_64-w64-mingw32-nm
else
    cross=(-G Ninja)
    objdump=objdump
    nm=nm
fi
# HPPS_NO_ART: no game artwork inside the exes given to others (now the case in every build)
# HPPS_FORTIFY=OFF: no _FORTIFY_SOURCE (a player's main-menu keyboard hitch on Windows, gone in a build without the
# release hardening, 2026-10-25); the stack canaries stay
cmake -S . -B build-release "${cross[@]}" -DCMAKE_BUILD_TYPE=Release -DHPPS_RELEASE=ON -DHPPS_NO_ART=ON -DHPPS_FORTIFY=OFF -DHPPS_VERSION="${VERSION:-0.4.0}" \
      > logs/build-release-config.log
nice -n 10 cmake --build build-release -j"$(nproc)" --target hpps hpps_launcher > logs/build-release.log 2>&1 ||
    { tail -30 logs/build-release.log; exit 1; }

dist="$port/dist/hpps-$(date +%Y%m%d)"
# Never delete the folder: players (and testers) drop game/, settings and saves into it. Only the two executables
# are replaced; anything else found there is left alone. (2026-10-20 an rm -rf of this folder destroyed a game/
# folder that had been moved into it for a test.)
mkdir -p "$dist"
cp -f build-release/hpps.exe build-release/hpps_launcher.exe "$dist/"
# players' readme: requirements, installing, controls, the F10 options; and what changed since the last release
cp -f README.txt "$dist/"
cp -f "$port/../CHANGES.md" "$dist/CHANGES.txt"
# licence texts: the LGPL code from xemu (src/apu, src/nv2a/nv2a_regs.h) is linked into hpps.exe, and the LGPL
# requires a copy of the licence with every binary; README.txt points to the source
mkdir -p "$dist/licenses"
cp -f "$port/../LICENSE" "$dist/licenses/LICENSE.txt"
cp -f "$port/../NOTICE" "$dist/licenses/NOTICE.txt"
cp -f "$port/../LICENSES/xboxrecomp-MIT.txt" "$dist/licenses/xboxrecomp-MIT.txt"
cp -f "$port/../LICENSES/LGPL-2.1.txt" "$dist/licenses/LGPL-2.1.txt"
# the MIT code built into the executables keeps its notices too: SCDA's renderer and Dear ImGui
cp -f "$port/src/scda/LICENSE.SCDA" "$dist/licenses/SCDA-XBERecomp.txt"
cp -f "$port/third_party/imgui/LICENSE.txt" "$dist/licenses/DearImGui.txt"
# the launcher title's font, Magic School One (FontMesa): its own terms, from its download
cp -f "$port/src/launcher/font/MagicSchool-licensing.txt" "$dist/licenses/MagicSchool-font.txt"

# res/base_shaders.bin ships as shadercache\base_shaders.bin: a player's first visit to a level then needs no shader
# compiler, only pipelines; one their driver refuses is compiled on their PC instead. Shader keys are the generated
# HLSL's hash, the same on every PC (checked 2026-10-20: 80 of 91 Windows shaders matched a cache built elsewhere).
# Refresh it from a play-through's shadercache\shaders.bin (copy it over res/base_shaders.bin) when shader
# generation changes, or the old entries simply stop matching.
if [ -s res/base_shaders.bin ]; then
    mkdir -p "$dist/shadercache"
    cp -f res/base_shaders.bin "$dist/shadercache/base_shaders.bin"
else
    echo "warning: res/base_shaders.bin missing, no base shaders shipped" >&2
fi

# the security bits every player build must carry
for exe in "$dist"/*.exe; do
    bits=$("$objdump" -p "$exe" | grep -A4 DllCharacteristics)
    for want in HIGH_ENTROPY_VA DYNAMIC_BASE NX_COMPAT; do
        grep -q "$want" <<< "$bits" || { echo "$exe: $want missing" >&2; exit 1; }
    done
done
[ "$("$nm" "$dist/hpps.exe" | grep -c __stack_chk_fail)" -gt 0 ] || { echo "hpps.exe: no stack protector" >&2; exit 1; }

# every DLL either exe imports must come with Windows 10/11 itself (the C/C++ runtimes are linked in statically);
# anything else would have to be shipped in the folder
builtin='^(api-ms-win-crt-.*|kernel32|user32|gdi32|shell32|ole32|winmm|dwmapi|dbghelp|d3d11|d3d12|dxgi|d3dcompiler_47|xaudio2_8|xinput1_4|mfplat|mfreadwrite|windowscodecs|imm32|comctl32|advapi32)\.dll$'
for exe in "$dist"/*.exe; do
    "$objdump" -p "$exe" | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u | while read -r dll; do
        grep -Eq "$builtin" <<< "$dll" || { echo "$(basename "$exe") needs $dll, which Windows does not ship" >&2; exit 1; }
    done
done

ls -la "$dist"

# the zip players download: the folder's contents at its top, as v0.1.0's (VERSION=0.2.0 tools/package_release.sh)
if [ -n "${VERSION:-}" ]; then
    zip="$port/dist/hpps_v${VERSION}_windows.zip"
    rm -f "$zip"
    python3 - "$dist" "$zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, src))
PY
    ls -la "$zip"
fi
