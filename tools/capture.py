#!/usr/bin/env python3
"""Load a capture (the F6 key: docs/runtime.md, "Tools") headless and dump frames from it.

    tools/capture.py 12 [--frames 60] [--every 1] [--out DIR]     # by number, or a capture folder's path
    tools/capture.py 12 --show                                     # what it holds, no run

The run: a new process, in a temporary directory, booted as far as frame 30 and then the capture's state1.bin loaded
(MHFU_STATEDIR, MHFU_STATE_LOAD), with the capture's MHFU_ settings (env.txt) but for where things go, an empty saves folder
of its own and no window. Frames are written as PNGs every --every game frames from the state's on, into --out (by default
frames/ in the capture's folder). Everything else about a capture is plain text in its folder.
"""
import argparse
import glob
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEFT_OUT = {"MHFU_REPLAY", "MHFU_RECORD", "MHFU_STATEDIR", "MHFU_SAVEDIR", "MHFU_CAPTUREDIR", "MHFU_HEADLESS", "MHFU_DUMP", "MHFU_DUMP_FROM",
            "MHFU_DUMP_AT", "MHFU_DUMP_PRESENT", "MHFU_STATE_LOAD", "MHFU_STATE_SAVE", "MHFU_STATE_TEST", "MHFU_RAM_HASH", "MHFU_FULLSCREEN"}


def find(which):
    if os.path.isdir(which):
        return os.path.abspath(which)
    base = os.environ.get("MHFU_CAPTUREDIR")
    if not base:
        data = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
        base = os.path.join(data, "mhfu-recomp", "captures")
    path = os.path.join(base, f"{int(which):04d}")
    if os.path.isfile(path):            # (moved into a ticket: tools/tickets.py attach)
        path = open(path).read().strip()
    if not os.path.isdir(path):
        sys.exit(f"no capture at {path}")
    return path


def state_frame(folder):
    for line in open(os.path.join(folder, "info.txt")):
        if line.startswith("state state1.bin taken at frame "):
            return int(line.split()[5])
        if line.startswith("frame "):
            fallback = int(line.split()[1])
    return fallback


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="its number, or its folder")
    ap.add_argument("--frames", type=int, default=30, help="how many frames to dump")
    ap.add_argument("--every", type=int, default=1, help="dump every Nth game frame")
    ap.add_argument("--out", help="where the frames go (default: frames/ in the capture's folder)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "mhfu"))
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--show", action="store_true", help="print what the capture holds, and stop")
    args = ap.parse_args()
    folder = find(args.capture)
    if args.show:
        for f in sorted(os.listdir(folder)):
            print(f"{os.path.getsize(os.path.join(folder, f)):>10}  {f}")
        print(open(os.path.join(folder, "info.txt")).read().split("\nobjects")[0])
        return
    if not os.path.exists(os.path.join(folder, "state1.bin")):
        sys.exit(f"{folder} has no state1.bin (see the end of its info.txt)")
    out = os.path.abspath(args.out or os.path.join(folder, "frames"))
    os.makedirs(out, exist_ok=True)
    first = state_frame(folder)
    with tempfile.TemporaryDirectory(prefix="mhfu-capture-") as d:
        os.makedirs(os.path.join(d, "build"))
        os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
        os.makedirs(os.path.join(d, "cfg", "mhfu-recomp"))
        os.makedirs(os.path.join(d, "SAVEDATA"))
        with open(os.path.join(d, "cfg", "mhfu-recomp", "settings.ini"), "w") as f:
            f.write(f"iso = {args.iso}\n")
        env = {k: v for k, v in os.environ.items() if not k.startswith("MHFU_")}
        for line in open(os.path.join(folder, "env.txt")):
            k, _, v = line.rstrip("\n").partition("=")
            if k not in LEFT_OUT and not k.startswith("MHFU_TEST_"):
                env[k] = v
        env.update(XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_STATEDIR=folder,
                   MHFU_STATE_LOAD="30,1", MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_DUMP=str(args.every),
                   MHFU_DUMP_FROM=str(first + 1), MHFU_CAPTUREDIR=os.path.join(d, "captures"))
        log = open(os.path.join(out, "log.txt"), "w")
        print(f"loading {folder} (state from frame {first}) with {os.path.abspath(args.bin)}")
        p = subprocess.Popen([os.path.abspath(args.bin)], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
        frames_dir = os.path.join(d, "build", "frames")
        end = time.time() + args.timeout
        done = False
        while time.time() < end and p.poll() is None:
            have = len(glob.glob(os.path.join(frames_dir, "*.png")))
            if have > args.frames:         # (the last may be half written)
                done = True
                break
            time.sleep(0.2)
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        names = sorted(glob.glob(os.path.join(frames_dir, "*.png")))[:args.frames]
        for name in names:
            shutil.move(name, os.path.join(out, os.path.basename(name)))
    log.close()
    text = open(os.path.join(out, "log.txt"), errors="replace").read()
    if "state 1 loaded" not in text:
        print("the state did not load: see", os.path.join(out, "log.txt"))
        sys.exit(1)
    print(f"{len(names)} frames in {out}" + ("" if done else " (stopped early: see log.txt)"))
    if not done:
        sys.exit(1)


if __name__ == "__main__":
    main()
