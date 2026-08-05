#!/bin/bash
# Der EINZIGE richtige Weg, den Windows-Editor auszuliefern.
#
# Warum es dieses Skript gibt: auf dem Webhost (LXC 112) laeuft alle 5 Minuten
# ein Cron (/usr/local/bin/daidalos-sync.sh). Der holt sich
#     http://192.168.1.54:8000/raw/deploy/dai_editor.exe
# und schreibt daraus /var/www/daidalos/download/DaidalosEditor.exe plus
# api/version.json. Wer stattdessen die Datei direkt in den Webordner kopiert,
# sieht sie spaetestens 5 Minuten spaeter wieder durch den ALTEN Build ersetzt -
# genau so ist am 03.08.2026 der kaputte Build wieder live gegangen.
#
# Quelle der Wahrheit ist also /root/jarvis/client/deploy/dai_editor.exe.
#
#   tools/deploy_editor.sh            # baut nicht, liefert nur aus
#   BUILD=1 tools/deploy_editor.sh    # baut vorher build_win.sh
set -e
cd "$(dirname "$0")/.."

EXE=build-win/editor_demo.exe
DEPLOY=/root/jarvis/client/deploy/dai_editor.exe

[ "${BUILD:-0}" = "1" ] && ./build_win.sh

[ -f "$EXE" ] || { echo "!! $EXE fehlt - erst ./build_win.sh"; exit 1; }

SHA=$(sha256sum "$EXE" | cut -d' ' -f1)
SIZE=$(stat -c%s "$EXE")
cp "$EXE" "$DEPLOY"
cp "$EXE" "/root/jarvis/public/DaidalosEditor-$(date -u +%Y%m%d-%H%M).exe"

echo "-- deploy-Quelle gesetzt: $DEPLOY"
echo "   sha256 $SHA"
echo "   size   $SIZE"

# Gegenprobe: das ist genau die Datei, die der Cron gleich zieht.
GOT=$(curl -fsS http://127.0.0.1:8000/raw/deploy/dai_editor.exe | sha256sum | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "!! /raw/deploy liefert etwas anderes: $GOT"; exit 1; }
echo "-- /raw/deploy stimmt ueberein"

cat <<EOF

Der Cron auf LXC 112 uebernimmt das innerhalb von 5 Minuten von selbst.
Sofort live schalten (optional, vom JARVIS-Server aus):

  ssh -p 209 root@109.69.172.199 "curl -fsS -o /tmp/dai.exe \\
    http://192.168.1.54:8000/raw/deploy/dai_editor.exe && \\
    pct push 112 /tmp/dai.exe /var/www/daidalos/download/DaidalosEditor.exe --perms 644 && \\
    printf '{\"version\":\"$(date -u +%Y.%m.%d)-N\",\"sha256\":\"$SHA\",\"size\":$SIZE,\"updated\":\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\",\"url\":\"/download/DaidalosEditor.exe\"}' > /tmp/v.json && \\
    pct push 112 /tmp/v.json /var/www/daidalos/api/version.json --perms 644"

Danach IMMER gegenpruefen:
  curl -s https://daidalos.fleitec.com/api/version.json
  curl -s https://daidalos.fleitec.com/download/DaidalosEditor.exe | sha256sum
EOF
