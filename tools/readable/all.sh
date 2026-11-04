#!/bin/sh
# The readable view of the whole game: tools/readable/all.sh [out] (default build/read). A pass without signatures once, for its
# returns (signatures.py's hint for functions no direct call reaches); signatures from the machine code; then the view.
set -e
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/projects/mhfu-ghidra/work}
JOBS=${JOBS:-8}
out=${1:-$here/../../build/read}
py=$here/../.venv/bin/python
run_all() { cut -f1 "$WORK/modules.tsv" | CPUS=1 xargs -P "$JOBS" -I{} sh -c "timeout -s KILL 1800 '$here/run.sh' {} '$1' || echo 'FAILED {}'"; }
"$py" "$here/gen_types.py" "$WORK"
if [ ! -e "$WORK/signatures.decompiler.tsv" ]; then
    rm -f "$WORK/signatures.tsv"
    rm -rf "$WORK/pass"
    run_all "$WORK/pass"
    for d in "$WORK/pass"/*/; do
        m=$(basename "$d")
        awk -F'\t' -v m="$m" -v OFS='\t' '{ print m, $0 }' "$d/signatures.tsv"
    done > "$WORK/signatures.decompiler.tsv"
fi
"$py" "$here/signatures.py" "$WORK" "$WORK/signatures.decompiler.tsv"
rm -rf "$out"
run_all "$out"
"$py" "$here/gen_types.py" "$WORK" "$out/mhfu_types.h"
"$py" "$here/decimals.py" "$out"
