#!/bin/sh
# Decompile one module headless: tools/readable/run.sh <module> [out]. GHIDRA, WORK, CPUS and MEM may be set in the
# environment. Output (from the game: never commit it) goes to <out>/<module>/, the log to $WORK/logs/.
set -e
here=$(cd "$(dirname "$0")" && pwd)
GHIDRA=${GHIDRA:-$HOME/projects/mhfu-ghidra/ghidra_12.2_PUBLIC}
WORK=${WORK:-$HOME/projects/mhfu-ghidra/work}
CPUS=${CPUS:-2}
module=$1
out=${2:-$here/../../build/read}
base=$(awk -F'\t' -v m="$module" '$1 == m { print $2 }' "$WORK/modules.tsv")
[ -n "$base" ] || { echo "no module $module in $WORK/modules.tsv" >&2; exit 1; }
proj=$(mktemp -d "$WORK/proj.XXXXXX")
mkdir -p "$WORK/logs"
status=0
nice -n 10 "$GHIDRA/support/analyzeHeadless" "$proj" p \
    -import "$WORK/$module/image.bin" -loader BinaryLoader -loader-baseAddr "0x$base" \
    -processor Allegrex:LE:32:default -noanalysis -max-cpu "$CPUS" \
    -scriptPath "$here/ghidra_scripts" -postScript DecompileModule.java "$WORK" "$module" "$out" \
    > "$WORK/logs/$module.log" 2>&1 || status=$?
rm -rf "$proj"
exit $status
