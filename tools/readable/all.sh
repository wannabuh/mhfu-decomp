#!/bin/sh
# The readable view of the whole game: tools/readable/all.sh [out] (default build/read). Two passes over every module: the
# first finds each function's signature, the second decompiles with all of them known. JOBS modules at a time.
set -e
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/projects/mhfu-ghidra/work}
JOBS=${JOBS:-6}
out=${1:-$here/../../build/read}
pass1=$WORK/pass1
rm -f "$WORK/signatures.tsv"
rm -rf "$pass1"
cut -f1 "$WORK/modules.tsv" | CPUS=1 xargs -P "$JOBS" -I{} sh -c "timeout -s KILL 1800 '$here/run.sh' {} '$pass1' || echo 'FAILED pass 1 {}'"
for d in "$pass1"/*/; do
    m=$(basename "$d")
    awk -F'\t' -v m="$m" -v OFS='\t' '{ print m, $0 }' "$d/signatures.tsv"
done > "$WORK/signatures.tsv.new"
mv "$WORK/signatures.tsv.new" "$WORK/signatures.tsv"
rm -rf "$out"
cut -f1 "$WORK/modules.tsv" | CPUS=1 xargs -P "$JOBS" -I{} sh -c "timeout -s KILL 1800 '$here/run.sh' {} '$out' || echo 'FAILED pass 2 {}'"
