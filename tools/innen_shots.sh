#!/usr/bin/env bash
# INNEN M0, photographed from inside the game's first three rooms.
#
#   tools/innen_shots.sh [OUTDIR] [W] [H] [PREFIX]
#
# It starts tools/modeling_shot in --serve mode (headless, no window, no X),
# sends examples/scripts/innen_m0.js down the bridge - the same socket Jarvis
# uses - and then asks the bridge for four pictures from four camera poses.
# The rooms are NOT in any C++ file: change the JavaScript and these pictures
# change with it.
#
#   20-innen-zelle.png    standing in the phone box, looking out of its door
#   21-innen-flur.png     the hallway, down its length, into the dark end
#   22-innen-halle.png    the room that is too big, with the stairs to nowhere
#   23-innen-plan.png     all three from above the ceiling line
#   24-innen-spieler.png  from the player's own eyes, torch on
#
# It also saves the scene, so the editor can open exactly what was shot.
set -uo pipefail
cd "$(dirname "$0")/.."

OUTDIR=${1:-.gauntlet-shots}
W=${2:-1600}
H=${3:-900}
PREFIX=${4:-}
PORT=${DAI_INNEN_PORT:-8393}

if [ ! -x build/modeling_shot ]; then
    echo "innen_shots: build/modeling_shot missing - run tools/build_modeling_shot.sh first"
    exit 1
fi

mkdir -p "$OUTDIR"

DAI_SHADER_DIR=shaders ./build/modeling_shot /tmp "$W" "$H" \
    --serve 240 --bridge "$PORT" > /tmp/innen_serve.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

# Wait for the socket rather than sleeping a guess.
for _ in $(seq 1 120); do
    if python3 - "$PORT" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket(); s.settimeout(0.4)
sys.exit(0 if s.connect_ex(("127.0.0.1", int(sys.argv[1]))) == 0 else 1)
PY
    then break; fi
    sleep 0.5
done

B="tools/daibridge.py --port $PORT"

$B js -f examples/scripts/innen_m0.js > /tmp/innen_build.json || {
    echo "innen_shots: the room script failed"; cat /tmp/innen_serve.log | tail -5; exit 1; }
cat /tmp/innen_build.json

RC=0
shot() { # name eye target fov
    $B shot "$OUTDIR/${PREFIX}$1.png" --eye="$2" --target="$3" --fov "$4" >/dev/null || RC=1
}

# Eye height 1.55 m, which is where the player's camera is: a room that only
# reads from a drone shot is a room nobody has stood in.
shot 20-innen-zelle   "0,1.55,0.45"    "0,1.35,-3"       60
shot 21-innen-flur    "0,1.65,-1.6"    "0,1.45,-11"      55
shot 22-innen-halle   "0,1.70,-10.2"   "-2.5,1.6,-15.5"  60
# From the player's eyes: the same pose the behaviour puts the camera in, read
# out of the scene rather than typed here, so a player that spawns somewhere
# else changes the picture. The torch is a spot light in the document, so it
# lights this shot even though no behaviour is running - which is the point of
# the light being a NODE and not something a script invents.
EYE=$($B js "var p=node.getPos(scene.find('Spieler')); JSON.stringify([p[0],p[1]+0.62,p[2]])" 2>/dev/null | tr -d '[]" ')
if [ -n "$EYE" ]; then
    shot 24-innen-spieler "$EYE" "0,1.35,-6" 75
fi

# The plan view is the only one taken with the ceilings switched off: with
# them on it is a photograph of three lids, which says nothing about the rooms
# underneath. They go back on before the scene is saved, so the file on disk is
# the level and not the picture.
$B js "['Zelle','Flur','Halle'].forEach(function(r){var c=scene.find(r+'.Ceiling'); if(c) node.setNum(c,'renderer.enabled',0);}); 'off'" >/dev/null || RC=1
shot 23-innen-plan    "7.5,9.5,-3.5"   "0,0.5,-8.5"      48
$B js "['Zelle','Flur','Halle'].forEach(function(r){var c=scene.find(r+'.Ceiling'); if(c) node.setNum(c,'renderer.enabled',1);}); 'on'" >/dev/null || RC=1

$B save projects/Untitled/scenes/innen_m0.daiscene >/dev/null || RC=1
$B quit >/dev/null 2>&1

ls -1 "$OUTDIR/${PREFIX}"2*-innen-*.png 2>/dev/null
exit $RC
