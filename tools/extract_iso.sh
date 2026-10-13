#!/bin/sh
# Extract a game ISO into game/ (ignored by git) and check it is the US release (ULUS10391).
# Usage: tools/extract_iso.sh <path/to/game.iso>
set -e
iso=${1:?usage: tools/extract_iso.sh <game.iso>}
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/game"
if [ -e "$out" ] && [ -n "$(ls -A "$out" 2>/dev/null)" ]; then
    echo "game/ is not empty - remove it first to extract again" >&2
    exit 1
fi
mkdir -p "$out"
7z x -y -o"$out" "$iso" >/dev/null
"$root/tools/.venv/bin/python" "$root/tools/sfo.py" "$out/PSP_GAME/PARAM.SFO"
id=$("$root/tools/.venv/bin/python" "$root/tools/sfo.py" --get DISC_ID "$out/PSP_GAME/PARAM.SFO")
if [ "$id" != "ULUS10391" ]; then
    echo "warning: disc ID is $id, not ULUS10391 (the version this project targets)" >&2
fi
echo "extracted to game/ ($(du -sh "$out" | cut -f1))"
