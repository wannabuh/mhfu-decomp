#!/usr/bin/env python3
"""The picture's regression test: real windows of frames, at the player's settings, measured and laid side by side.

Each scene is played headless (a save state, or a recording up to a frame) and every frame shown is dumped. Then:

  flicker   per tile of the frame (16 by 9), how far each frame stands out from the two beside it, in levels of 0-255,
            less how far those two are from each other: what is there one frame and not the next counts, whether it
            alternates (a sky flipping between two pictures) or comes now and then (a third of the frames losing the
            ground); the camera's turning hardly does (what changes steadily does not stand out). Over --limit (default
            6) at the 95th percentile of the frames: FLICKER, and the exit status is 1. Its maximum alone can be an
            effect (a puff of dust), and a fast turn of the camera measures high as well (at MHFU_FPS=0 most): with
            --base, the limit is over the base's own measure in the same window; see the images.
  against   with --base BIN, the same scene from another build: the mean difference and the share of pixels off by
            over 24, frame by frame. Reported, never judged: which of two pictures is right takes a look.
  images    <out>/<scene>-seq.png, the six frames about the worst one; <out>/<scene>-cmp.png, rows of base,
            this build, and the difference times 4. Look at them: the numbers only say where.

    tools/picture_check.py BIN [--base BIN] [--scenes village,snow,ice-cave,camp] [--frames 120] [--out DIR]
                           [--states DIR] [--env MHFU_FPS=0 ...] [--base-env MHFU_SHADOW_EXTRAS=0 ...]

The scene "village-state" is slot 1 of --states (it needs a build of the state's layout). Settings are the player's
(~/.config/mhfu-recomp/settings.ini) unless --config says otherwise, read and never written, as are the recordings and
saves in tests/data: each run has its own folder and copy of the saves.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tests", "data")
GAME = os.path.join(ROOT, "game")

# name: recording, the frame the window starts at (where the recording has the place in view), its MHFU_TICK_HZ if not
# the player's (camp.rec was made at 30: as tools/suite.py plays it)
SCENES = {
    "village": ("overnight-shops.rec", 2900, None),
    "snow": ("snow.rec", 7000, None),
    "ice-cave": ("ice-cave.rec", 4400, None),
    "camp": ("camp.rec", 7400, "60@7000"),
    "village-state": (None, 2800, None),
}
TILES = (9, 16)


def play(binary, scene, frames, d, a, extra=()):
    """The scene played by binary in the new folder d: its frames' paths"""
    rec, start, tick = SCENES[scene]
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "build"))
    os.symlink(os.path.realpath(GAME), os.path.join(d, "game"))
    env = dict(os.environ, MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_DUMP="1", MHFU_DUMP_FROM=str(start))
    if a.config:
        env["XDG_CONFIG_HOME"] = os.path.abspath(a.config)
    if tick:
        env["MHFU_TICK_HZ"] = tick
    if rec:
        shutil.copytree(os.path.join(DATA, "saves", "kit"), os.path.join(d, "SAVEDATA"), symlinks=True)
        env.update(MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_STATEDIR=os.path.join(d, "states"),
                   MHFU_REPLAY=os.path.realpath(os.path.join(DATA, "rec", rec)),
                   MHFU_REPLAY_SPEED=str(max(start - 60, 1)))
    else:
        if not a.states:
            return []
        shutil.copytree(os.path.join(a.states, "..", "save"), os.path.join(d, "SAVEDATA"), symlinks=True)
        env.update(MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_STATEDIR=os.path.abspath(a.states),
                   MHFU_STATE_LOAD="30,1")
    for e in list(a.env) + list(extra):
        k, _, v = e.partition("=")
        env[k] = v
    out = os.path.join(d, "build", "frames")
    with open(os.path.join(d, "log.txt"), "w") as log:
        p = subprocess.Popen(["nice", "-n", "10", "timeout", "-s", "KILL", str(a.secs), os.path.abspath(binary)],
                             cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
        while p.poll() is None:
            if os.path.isdir(out) and len(os.listdir(out)) >= frames + 2:
                p.kill()
                break
            time.sleep(0.25)
        p.wait()
    time.sleep(0.2)
    return sorted(glob.glob(os.path.join(out, "*.png")))[:frames]


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def tile_means(frame):
    h, w = frame.shape[:2]
    th, tw = h // TILES[0], w // TILES[1]
    return frame[:th * TILES[0], :tw * TILES[1]].reshape(TILES[0], th, TILES[1], tw, 3).mean(axis=(1, 3, 4))


def flicker(paths):
    """Per frame, the worst tile's standing out from the two beside it: (values, tile means)"""
    means = np.stack([tile_means(load(p)) for p in paths])
    worst = np.zeros(len(paths))
    for i in range(1, len(paths) - 1):
        out = np.minimum(np.abs(means[i] - means[i - 1]), np.abs(means[i] - means[i + 1]))
        worst[i] = (out - np.abs(means[i - 1] - means[i + 1])).max()
    return worst, means


def sheet(rows, path):
    """Rows of images into one png, at most 640 wide each"""
    def fit(im):
        return im.resize((min(im.width, 480), im.height * min(im.width, 480) // im.width))
    rows = [[fit(i) for i in r] for r in rows]
    w = max(sum(i.width for i in r) for r in rows)
    h = sum(max(i.height for i in r) for r in rows)
    out = Image.new("RGB", (w, h))
    y = 0
    for r in rows:
        x = 0
        for i in r:
            out.paste(i, (x, y))
            x += i.width
        y += max(i.height for i in r)
    out.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("--base", help="another build, to compare with")
    ap.add_argument("--scenes", default="village,snow,ice-cave,camp")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--out", default=os.path.join(DATA, "runs", time.strftime("pictures-%Y%m%d-%H%M%S")))
    ap.add_argument("--states", help="a folder of save states (and ../save beside it), for village-state")
    ap.add_argument("--config", help="a folder for XDG_CONFIG_HOME, instead of the player's settings")
    ap.add_argument("--env", action="append", default=[], help="K=V for the runs (both builds)")
    ap.add_argument("--base-env", action="append", default=[], help="K=V for the base's runs only")
    ap.add_argument("--limit", type=float, default=6.0)
    ap.add_argument("--secs", type=int, default=180, help="a run's limit")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    bad, report = 0, {}
    for scene in a.scenes.split(","):
        new = play(a.binary, scene, a.frames, os.path.join(a.out, "run", scene, "new"), a)
        if len(new) < 8:
            print(f"{scene}: only {len(new)} frames: not measured (see {a.out}/run/{scene}/new/log.txt)")
            bad = 1
            continue
        worst, means = flicker(new)
        at = int(worst.argmax())
        top = means[:, :2].mean(axis=(1, 2))
        r = {"frames": len(new), "first": os.path.basename(new[0]), "flicker_max": round(float(worst.max()), 2),
             "flicker_p95": round(float(np.percentile(worst, 95)), 2), "worst_frame": os.path.basename(new[at]),
             "top_band": [round(float(top.min()), 1), round(float(top.max()), 1)],
             "top_band_step": round(float(np.abs(np.diff(top)).mean()), 2)}
        a0 = max(at - 2, 0)
        sheet([[Image.open(p).convert("RGB") for p in new[a0:a0 + 3]],
               [Image.open(p).convert("RGB") for p in new[a0 + 3:a0 + 6]]], os.path.join(a.out, f"{scene}-seq.png"))
        if a.base:
            old = play(a.base, scene, a.frames, os.path.join(a.out, "run", scene, "base"), a, a.base_env)
            n = min(len(old), len(new))
            if len(old) >= 8:
                base_p95 = float(np.percentile(flicker(old)[0], 95))
                r["base_flicker_p95"] = round(base_p95, 2)
            if n:
                diffs = [np.abs(load(old[i]) - load(new[i])) for i in range(n)]
                r["against_mean"] = round(float(np.mean([x.mean() for x in diffs])), 2)
                r["against_max_mean"] = round(float(max(x.mean() for x in diffs)), 2)
                r["against_over24"] = round(float(np.mean([(x.max(axis=2) > 24).mean() for x in diffs])) * 100, 2)
                rows = []
                for i in (0, n // 2, int(np.argmax([x.mean() for x in diffs]))):
                    rows.append([Image.open(old[i]).convert("RGB"), Image.open(new[i]).convert("RGB"),
                                 Image.fromarray(np.clip(diffs[i] * 4, 0, 255).astype(np.uint8))])
                sheet(rows, os.path.join(a.out, f"{scene}-cmp.png"))
            else:
                r["against"] = "the base gave no frames"
        verdict = "FLICKER" if r["flicker_p95"] > a.limit + r.get("base_flicker_p95", 0) else "steady"
        bad |= verdict != "steady"
        r["verdict"] = verdict
        report[scene] = r
        print(scene, json.dumps(r), flush=True)
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print("images and report in", a.out)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
