#!/usr/bin/env python3
"""The tracing build's evidence: trace.py OUT REC[:SAVES]... replays each recording headless with the build generated with
RECOMP_TRACE_ALL=1, writing OUT/<name>.trace (trace_hook.c), and its RAM hashes to OUT/<name>.hashes. SAVES is a directory
under tests/data/saves (default overnight). --compare DIR checks the hashes against those of replaycheck.py's run there."""
import argparse
import os
import re
import shutil
import subprocess
import time

ROOT = os.path.expanduser("~/projects/mhfu-recomp")
ISO = os.path.expanduser("~/Games/psp/mhfu.iso")
BIN = os.path.expanduser("~/projects/mhfu-ghidra/tracebuild/build/mhfu")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out")
    ap.add_argument("recs", nargs="+")
    ap.add_argument("--compare")
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    todo, running = list(args.recs), []
    while todo or running:
        while todo and len(running) < args.j:
            rec, _, saves = todo.pop(0).partition(":")
            name = os.path.basename(rec)[:-4]
            d = os.path.join(args.out, "run-" + name)
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(os.path.join(d, "build"))
            os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
            shutil.copytree(os.path.join(ROOT, "tests/data/saves", saves or "overnight"), os.path.join(d, "SAVEDATA"))
            os.makedirs(os.path.join(d, "cfg/mhfu-recomp"))
            with open(os.path.join(d, "cfg/mhfu-recomp/settings.ini"), "w") as f:
                f.write(f"iso = {ISO}\n")
            env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_FPS="0",
                       MHFU_SCALE="1", MHFU_TICK_HZ="30", MHFU_REPLAY=os.path.abspath(rec),
                       MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_STATEDIR=os.path.join(d, "states"),
                       MHFU_RAM_HASH="300", MHFU_TRACE_ALL=os.path.abspath(os.path.join(args.out, name + ".trace")))
            log = open(os.path.join(d, "log"), "w")
            running.append((name, d, subprocess.Popen(["nice", "-n", "5", BIN], cwd=d, env=env, stdout=log,
                                                      stderr=subprocess.STDOUT), time.time()))
        for r in list(running):
            name, d, p, t0 = r
            with open(os.path.join(d, "log"), errors="replace") as f:
                text = f.read()
            done = "replay done" in text
            if done or p.poll() is not None or time.time() - t0 > args.timeout:
                if p.poll() is None:
                    p.kill()
                p.wait()
                hashes = re.findall(r"ram hash \d+ \w+", text)
                with open(os.path.join(args.out, name + ".hashes"), "w") as f:
                    f.write("\n".join(hashes) + "\n")
                verdict = ""
                if args.compare:
                    ref = os.path.join(args.compare, name + ".0", "log")
                    if os.path.exists(ref):
                        with open(ref, errors="replace") as f:
                            want = re.findall(r"ram hash \d+ \w+", f.read())
                        n = min(len(want), len(hashes))
                        verdict = f", {n} hashes " + ("match" if n and want[:n] == hashes[:n] else "DIFFER")
                size = os.path.getsize(os.path.join(args.out, name + ".trace"))
                print(f"{name}: {'done' if done else 'NOT DONE'} in {time.time() - t0:.0f}s, trace {size >> 10} KB{verdict}",
                      flush=True)
                running.remove(r)
                shutil.rmtree(d, ignore_errors=True)
        time.sleep(1)


if __name__ == "__main__":
    main()
