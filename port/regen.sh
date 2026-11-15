#!/usr/bin/env bash
# Disassemble + recompile an edition's default.xbe (run from anywhere). Every hand-written override file is listed with
# --exclude-manual so its sub_X keeps the generated body as sub_X_gen.
#
#   port/regen.sh        PAL (Europe) game/default.xbe        -> port/src/recomp/gen       (hpps.exe)
#   port/regen.sh ntsc   NTSC-US game/ntsc/default.xbe        -> port/src/recomp/gen_ntsc  (hpps_ntsc.exe)
#
# The hand-written code is written with PAL addresses. For NTSC the override files, the icall seeds and patch_gen.py
# are first translated by port/tools/edition/pal2ntsc.py (its map is remade from both XBEs and the PAL function list
# the PAL regen leaves in port/tools/edition/pal_functions.json), the same translation the build compiles.
set -e
cd "$(dirname "$0")/.."
EDITION=${1:-pal}
MANUAL="port/src/hle_dsound.c port/src/hle_video.c port/src/hle_input.c port/src/hle_xapi.c port/src/hle_debug.c
        port/src/hle_anim.c port/src/hle_trainer.c port/src/hle_frontend.c port/src/hle_camera.c
        port/src/scda/xdk/hle_d3d8.cpp"
E=port/tools/edition
case "$EDITION" in
pal)
    XBE=game/default.xbe GEN=port/src/recomp/gen SRC=.
    ;;
ntsc)
    XBE=game/ntsc/default.xbe GEN=port/src/recomp/gen_ntsc SRC=port/build-ntsc-src
    [ -f $E/pal_functions.json ] || { echo "regen.sh ntsc: run port/regen.sh (PAL) first: it leaves $E/pal_functions.json"; exit 1; }
    python3 $E/pal2ntsc.py map game/default.xbe $XBE $E/pal_functions.json -o $E/pal2ntsc_map.json
    python3 $E/pal2ntsc.py translate $E/pal2ntsc_map.json --pal game/default.xbe --rules $E/rules.txt --strict --quiet \
        --root . --out-dir $SRC $MANUAL port/icall_seeds.json port/tools/patch_gen.py
    ;;
*)
    echo "usage: port/regen.sh [pal|ntsc]"; exit 1 ;;
esac
EXCLUDE=()
for f in $MANUAL; do EXCLUDE+=(--exclude-manual "$SRC/$f"); done
# the XBE's layout (sections, imports, entry), which disasm reads as <xbe folder>/default_analysis.json
python3 -m tools.xbe_parser $XBE --json "$(dirname $XBE)/default_analysis.json"
python3 -m tools.disasm $XBE --text-only --extra-sections D3D,D3DX,XGRPH,DSOUND,XMV,XPP \
    --analysis-json "$(dirname $XBE)/default_analysis.json" --seed-functions $SRC/port/icall_seeds.json
[ "$EDITION" = pal ] && cp tools/disasm/output/functions.json $E/pal_functions.json
python3 -m tools.func_id $XBE
python3 -m tools.abi_analysis $XBE
python3 -m tools.recomp $XBE --all --split 1000 --gen-dir $GEN --game-name hpps "${EXCLUDE[@]}"
# hooks inside generated functions (debug traces) that an override file can't express
python3 $SRC/port/tools/patch_gen.py $GEN
