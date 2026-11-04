#!/bin/sh
# The readable view of the whole game: tools/readable/all.sh [out] (default build/read). Passes over every module, each with
# the signatures found so far, until they settle (at most PASSES); then the last with the final ones. JOBS modules at a time.
set -e
here=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/projects/mhfu-ghidra/work}
JOBS=${JOBS:-8}
PASSES=${PASSES:-5}
out=${1:-$here/../../build/read}
pass=$WORK/pass
py=$here/../.venv/bin/python
run_all() { cut -f1 "$WORK/modules.tsv" | CPUS=1 xargs -P "$JOBS" -I{} sh -c "timeout -s KILL 1800 '$here/run.sh' {} '$1' || echo 'FAILED {}'"; }
"$py" "$here/gen_types.py" "$WORK"
[ -n "$FINAL_ONLY" ] || rm -f "$WORK/signatures.tsv"
n=0
[ -z "$FINAL_ONLY" ] || n=$PASSES       # FINAL_ONLY=1: the view again with today's names and types, signatures as they are
while [ "$n" -lt "$PASSES" ]; do
    n=$((n + 1))
    rm -rf "$pass"
    run_all "$pass"
    result=$("$py" "$here/merge_sigs.py" "$pass" "$WORK/signatures.tsv")
    echo "pass $n: $result"
    changed=${result%% *}
    [ "$changed" -gt 20 ] || break
done
rm -rf "$out"
run_all "$out"
