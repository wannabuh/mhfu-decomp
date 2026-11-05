#!/usr/bin/env python3
"""Bugs put in hand-written code on purpose, one at a time, to see that build/handtest notices (docs/readable.md).

    handbugs.py readable/object_state.c ~/projects/mhfu-ghidra/work/bugs/object_state.txt [--worktree DIR] [--iterations N]

The source file is relative to the test worktree (one generated with RECOMP_TRACE_ALL=1), and is changed only there. The bugs file, kept
out of the repo, has a line per bug, what to find and what to put instead, with " -> " between them (with the spaces, so `self->x` is
safe). "only <name>" says which handtest case the bugs below it are for. Blank lines and lines starting with # are skipped:

    only object_state_is
    self->state == (uint8_t)c->r[6] -> self->state != (uint8_t)c->r[6]

The text to find must occur exactly once, so that it's clear what was changed: a bug that would change nothing, or could mean two
places, is refused before anything is built. Each bug is applied, handtest rebuilt and its case run in stub mode: CAUGHT if it reports a
difference or dies, MISSED if not, NO BUILD if it doesn't compile (which proves nothing). The file is put back afterwards, whatever
happens. Exit status 0 if none was missed. A missed bug still has to be explained: it may change nothing. """
import argparse
import os
import subprocess
import sys

FLAGS = "-fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation"
RUN_TIMEOUT = 300


def read_bugs(path, source):
    """[(case, old, new)], or exits saying what's wrong with the list"""
    bugs, problems, case = [], [], None
    with open(path) as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped.startswith("only "):
                case = stripped[5:].strip()
                continue
            if " -> " not in stripped:
                problems.append(f"line {number}: no ' -> ': {stripped}")
                continue
            old, new = stripped.split(" -> ", 1)
            if case is None:
                problems.append(f"line {number}: no 'only <name>' before it")
            elif old == new:
                problems.append(f"line {number}: changes nothing: {stripped}")
            elif source.count(old) != 1:
                problems.append(f"line {number}: found {source.count(old)} times, not once: {old}")
            else:
                bugs.append((case, old, new))
    if problems:
        sys.exit(f"{path}:\n  " + "\n  ".join(problems))
    return bugs


class Test:
    def __init__(self, args):
        self.dir = os.path.expanduser(args.worktree)
        self.snapshot = os.path.expanduser(args.snapshot)
        self.signatures = os.path.expanduser(args.signatures)
        self.iterations = str(args.iterations)

    def build(self):
        """the compiler's complaint if it doesn't build, or None"""
        done = subprocess.run(["make", "-j8", "build/handtest", f"HAND_FLAGS={FLAGS}", "HAND_LDFLAGS=-fprofile-instr-generate"],
                              cwd=self.dir, env={**os.environ, "RECOMP_TRACE_ALL": "1"}, capture_output=True, text=True, timeout=600)
        return None if done.returncode == 0 else done.stdout + done.stderr

    def run(self, case):
        """"same", or what shows that it isn't. Exits if the case doesn't exist: that would look like a bug missed."""
        command = ["build/handtest", self.snapshot, self.iterations, "stubs", self.signatures, "only", case]
        try:
            done = subprocess.run(command, cwd=self.dir, env={**os.environ, "LLVM_PROFILE_FILE": os.devnull},
                                  capture_output=True, text=True, timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            return "hung"
        if done.returncode < 0:
            return f"died of signal {-done.returncode}"
        lines = done.stdout.splitlines()
        if "fatal:" in done.stdout + done.stderr:     # a call to nowhere, by rt_fatal: not a case missing
            return "fatal"
        if not any(case in line and line.endswith(": same") or line.endswith(": FAILED") for line in lines):
            sys.exit(f"no case called {case}, or handtest couldn't run:\n{done.stdout}{done.stderr}")
        if done.returncode == 0:
            return "same"
        return "calls differ" if any(line.endswith("calls differ") for line in lines) else "RAM or result differs"


def main():
    parser = argparse.ArgumentParser(description="Bugs put in hand-written code on purpose: does handtest notice?")
    parser.add_argument("source", help="relative to the test worktree")
    parser.add_argument("bugs", help="the list of bugs (see this script's start)")
    parser.add_argument("--worktree", default="~/projects/mhfu-ghidra/typebuild")
    parser.add_argument("--snapshot", default="~/projects/mhfu-ghidra/work/snaps/overnight-fight1-004500.bin")
    parser.add_argument("--signatures", default="~/projects/mhfu-ghidra/work/signatures.tsv")
    parser.add_argument("--iterations", type=int, default=400)
    args = parser.parse_args()

    test = Test(args)
    path = os.path.join(test.dir, args.source)
    with open(path) as f:
        original = f.read()
    bugs = read_bugs(os.path.expanduser(args.bugs), original)

    # Without a clean start, every bug would look caught
    complaint = test.build()
    if complaint:
        sys.exit("doesn't build as it is:\n" + complaint)
    for case in sorted({bug[0] for bug in bugs}):
        result = test.run(case)
        if result != "same":
            sys.exit(f"{case} already differs without a bug: {result}")

    results = []
    try:
        for case, old, new in bugs:
            with open(path, "w") as f:
                f.write(original.replace(old, new, 1))
            complaint = test.build()
            result = test.run(case) if not complaint else None
            verdict = "NO BUILD" if complaint else "MISSED" if result == "same" else "CAUGHT"
            results.append((verdict, case, old, new))
            print(f"{verdict:8} {case}: {old} -> {new}" + (f"  ({result})" if verdict == "CAUGHT" else ""), flush=True)
    finally:
        with open(path, "w") as f:
            f.write(original)
    test.build()        # handtest as the source is again

    print(f"\n{len(results)} bugs: " + ", ".join(f"{sum(r[0] == v for r in results)} {v.lower()}" for v in ("CAUGHT", "MISSED", "NO BUILD")))
    for verdict, case, old, new in results:
        if verdict != "CAUGHT":
            print(f"  {verdict} {case}: {old} -> {new}")
    return 1 if any(r[0] == "MISSED" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
