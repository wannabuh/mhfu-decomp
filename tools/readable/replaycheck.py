#!/usr/bin/env python3
"""Check recordings replay deterministically: replaycheck.py WORK REC... plays each twice, headless, from fresh copies of
tests/data/saves/overnight, and compares the RAM hashes printed along the way. Both use the Vulkan renderer, as the recordings
were played (RAM parts from the soft renderer's around frame 1200), and the first dumps a picture every 600 frames, to
see the replay does what was played."""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.expanduser("~/projects/mhfu-recomp")
ISO = os.path.expanduser("~/Games/psp/mhfu.iso")


def start(work, rec, n):
    name = os.path.basename(rec)[:-4]
    d = os.path.join(work, f"{name}.{n}")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "build"))
    os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
    shutil.copytree(os.path.join(ROOT, "tests/data/saves/overnight"), os.path.join(d, "SAVEDATA"))
    os.makedirs(os.path.join(d, "cfg/mhfu-recomp"))
    with open(os.path.join(d, "cfg/mhfu-recomp/settings.ini"), "w") as f:
        f.write(f"iso = {ISO}\n")
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_FPS="0",
               MHFU_SCALE="1", MHFU_TICK_HZ="30", MHFU_REPLAY=rec,
               MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_STATEDIR=os.path.join(d, "states"), MHFU_RAM_HASH="300")
    if n == 0:
        env["MHFU_DUMP"] = "600"     # with the Vulkan renderer: the soft one's pictures are blank
    log = open(os.path.join(d, "log"), "w")
    return name, n, d, subprocess.Popen(["nice", "-n", "5", os.path.join(ROOT, "build/mhfu")], cwd=d, env=env,
                                        stdout=log, stderr=subprocess.STDOUT)


def main():
    work, recs = sys.argv[1], sys.argv[2:]
    runs = [start(work, r, n) for r in recs for n in (0, 1)]
    t0 = time.time()
    pending = list(runs)
    while pending and time.time() - t0 < 1500:
        for r in list(pending):
            with open(os.path.join(r[2], "log"), errors="replace") as f:
                done = "replay done" in f.read()
            if done or r[3].poll() is not None:
                print(f"{r[0]}.{r[1]}: {'done' if done else 'EXITED ' + str(r[3].poll())} at {time.time() - t0:.0f}s",
                      flush=True)
                if r[3].poll() is None:
                    r[3].kill()
                r[3].wait()
                pending.remove(r)
        time.sleep(2)
    for r in pending:
        print(f"{r[0]}.{r[1]}: TIMEOUT")
        r[3].kill()
    for rec in recs:
        name = os.path.basename(rec)[:-4]
        logs = [open(os.path.join(work, f"{name}.{n}", "log"), errors="replace").read() for n in (0, 1)]
        hashes = [re.findall(r".*hash.*", x, re.I) for x in logs]
        n = min(map(len, hashes))
        same = hashes[0][:n] == hashes[1][:n]
        print(f"{name}: {len(hashes[0])}/{len(hashes[1])} hashes, {'identical' if same and n else 'DIFFER or none'}")
        if hashes[0]:
            print("  last:", hashes[0][-1][:100])


if __name__ == "__main__":
    main()
