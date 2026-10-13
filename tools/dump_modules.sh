#!/bin/sh
# Decrypt the game's modules (EBOOT.BIN and the PRXs it loads) into game/decrypted/, by booting the ISO in PPSSPP for a
# few seconds with its "dump decrypted files" setting on. PPSSPP runs offscreen with a throwaway config directory, so
# your own PPSSPP settings are not touched. (PPSSPPHeadless ignores ppsspp.ini, so this uses the SDL build.)
# Usage: tools/dump_modules.sh <path/to/game.iso>
set -e
iso=$(realpath "${1:?usage: tools/dump_modules.sh <game.iso>}")
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/game/decrypted"
home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
mkdir -p "$home/.config/ppsspp/PSP/SYSTEM"
# DumpFileTypes is a bitmask: 1 = EBOOT, 2 = PRX
printf '[General]\nDumpFileTypes = 3\n' > "$home/.config/ppsspp/PSP/SYSTEM/ppsspp.ini"
HOME="$home" XDG_CONFIG_HOME="$home/.config" SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
    timeout 20 PPSSPPSDL "$iso" > "$home/ppsspp.log" 2>&1 || true
dump="$home/.config/ppsspp/PSP/SYSTEM/DUMP"
if [ ! -f "$dump/ULUS10391_EBOOT.BIN" ]; then
    echo "PPSSPP did not dump EBOOT.BIN; its log:" >&2
    tail -20 "$home/ppsspp.log" >&2
    exit 1
fi
mkdir -p "$out"
cp "$dump/ULUS10391_EBOOT.BIN" "$out/EBOOT.elf"
for prx in "$dump"/ULUS10391_*.prx; do
    [ -e "$prx" ] && cp "$prx" "$out/${prx##*/ULUS10391_}"
done
ls -l "$out"
