#!/usr/bin/env python3
"""Does the hunter do at 60 game frames a second what it does at 30? A recording played at both rates, compared.

A recording is played at its own rate and, converted (its frames halved or doubled, the mouse's moves summed or
spread), at the other - the input reaches the game at its 30ths either way, so the two runs do the same. Each logs the
hunter (MHFU_NATIVE_TRACE: its 8 KB, each game frame). They are compared action by action (the hunter's action at
+0x684, the k-th of each in either run), not frame by frame, so that a little drift between them does not matter:

  - an action that lasts longer or shorter at 60 (in 30ths of a second);
  - a field counting game frames: one that steps by 1 every frame or so at 30 and twice as often at 60 (or half);
  - the ground an action covers, more or less at 60.

    tools/ratecheck.py ledge.rec --rate 60 [--saves DIR] [--bin build/mhfu] [--keep DIR]

--rate is the rate the recording was made at (the user's tick_hz; 30 if none). Runs headless, mute, without the
user's settings, on a copy of the saves (the recording's: a replay made with other saves goes astray). Exits 1 if
anything is found. The fields flagged are offsets in the hunter object: tools/watch.py finds what writes them (the
object's address is in the trace's "# scene ... object ..." lines, kept with --keep).
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORDS = 0x800                       # the hunter's words logged: 0x2000 bytes
ACTION = 0x684


# -- recordings ------------------------------------------------------------------------------------------------------

def convert(src, dst, factor):
    """A recording at another rate: factor 0.5 halves its frames (60 -> 30: frame f -> (f + 1) // 2, the frames that
    fall together one, its buttons and stick the last's, its mouse their sum), 2 doubles them (f -> 2f - 1)"""
    def at(f):
        return (f + 1) // 2 if factor < 1 else 2 * f - 1
    head, inputs, pointers, end, camera = [], {}, [], None, None
    for line in open(src):
        p = line.split()
        if p and p[0].isdigit() and len(p) >= 6:
            f = at(int(p[0]))
            b, sx, sy, mx, my = p[1], p[2], p[3], float(p[4]), float(p[5])
            if f in inputs:
                o = inputs[f]
                inputs[f] = [b, sx, sy, o[3] + mx, o[4] + my]
            else:
                inputs[f] = [b, sx, sy, mx, my]
        elif p and p[0] == "pointer":
            pointers.append(f"pointer {at(int(p[1]))} {' '.join(p[2:])}")
        elif p and p[0] == "end":
            end = at(int(p[1]))
        elif p and p[0] == "camera":
            camera = at(int(p[1])) if int(p[1]) else 0
        elif line.strip():
            head.append(line.rstrip("\n"))
    with open(dst, "w") as o:
        for line in head:
            print(line, file=o)
        if camera is not None:
            print(f"camera {camera}", file=o)
        for f in sorted(inputs):
            b, sx, sy, mx, my = inputs[f]
            print(f"{f} {b} {sx} {sy} {mx:g} {my:g}", file=o)
        for line in pointers:
            print(line, file=o)
        if end is not None:
            print(f"end {end}", file=o)


def run(name, rec, rate, args, work):
    """The game played through rec at rate (headless, its own settings, a copy of the saves), the hunter logged:
    the trace's path"""
    d = os.path.join(work, name)
    os.makedirs(os.path.join(d, "build"))
    os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
    shutil.copytree(args.saves, os.path.join(d, "SAVEDATA"))
    os.makedirs(os.path.join(d, "cfg", "mhfu-recomp"))
    with open(os.path.join(d, "cfg", "mhfu-recomp", "settings.ini"), "w") as f:
        f.write(f"iso = {args.iso}\n")
    trace = os.path.join(d, "trace")
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_FPS="0",
               MHFU_RENDERER="soft", MHFU_SCALE="1", MHFU_TICK_HZ=str(rate), MHFU_REPLAY=os.path.abspath(rec),
               MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_NATIVE_TRACE=trace,
               MHFU_TRACE_WORDS=f"0,{WORDS:x}")
    log = open(os.path.join(d, "log"), "w")
    p = subprocess.Popen([os.path.abspath(args.bin)], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
    return p, trace, os.path.join(d, "log")


def wait(p, logpath, timeout):
    """Until the replay is done (the game then plays on: stopped)"""
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < timeout:
        with open(logpath) as f:
            if "replay done" in f.read():
                break
        time.sleep(1)
    done = p.poll() is None
    p.kill()
    p.wait()
    return done


# -- traces ----------------------------------------------------------------------------------------------------------

def load(path):
    """A trace's frames: (game time in 30ths, a 30th's frame, position, the hunter's words)"""
    out = []
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        if len(p) < 8 + WORDS:
            continue
        w = [int(x, 16) for x in p[-WORDS:]]
        out.append((float(p[0]), int(p[1]), (float(p[2]), float(p[3]), float(p[4])), w))
    return out


def segments(frames):
    """The runs of frames with the same action: (action, first index, last index)"""
    out, start = [], 0
    for i in range(1, len(frames) + 1):
        if i == len(frames) or frames[i][3][ACTION // 4] != frames[start][3][ACTION // 4]:
            out.append((frames[start][3][ACTION // 4], start, i - 1))
            start = i
    return out


def counter_steps(words, a, b):
    """Every field over frames a..b (words: the run's frames x words, uint32): for each size (4, 2, 1 bytes), how many
    frames each field moved by 1 (or -1), and by anything - arrays by field (offset / size)"""
    w = words[a:b + 1]
    # (a word that holds a float - an exponent of a usable size, on most frames - is no counter, nor its bytes: a
    # float changing a little steps its lowest byte by 1)
    exp = (w >> 23) & 0xFF
    floaty = ((exp >= 0x60) & (exp <= 0x9F)).mean(axis=0) > 0.8
    out = {}
    for size, view in ((4, w), (2, w.view(np.uint16)), (1, w.view(np.uint8))):
        d = np.diff(view.astype(np.int64), axis=0) & ((1 << (8 * size)) - 1)
        moves = (d != 0).sum(axis=0)
        is_one = (d == 1) | (d == (1 << (8 * size)) - 1)
        ones = is_one.sum(axis=0)
        ones[np.repeat(floaty, 4 // size)] = 0
        # the frames from its first step by 1 to its last: what it counted over (an action may wait on after)
        first = np.argmax(is_one, axis=0)
        last = is_one.shape[0] - 1 - np.argmax(is_one[::-1], axis=0)
        span = np.where(ones > 0, last - first + 1, 1)
        out[size] = (ones, moves, span)
    return out


def path_length(frames, a, b):
    s = 0.0
    for i in range(a + 1, b + 1):
        p, q = frames[i - 1][2], frames[i][2]
        s += ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5
    return s


def parting(f30, f60, apart=150.0, for_30ths=60, slack=5):
    """The game time (30ths) from which the runs' hunters are apart - more than apart units from wherever the other was
    within slack 30ths, for for_30ths 30ths on end: the input then does other things in either, and what is compared
    stops there. None if they keep together"""
    at60 = {}
    for t, _, p, _ in f60:
        at60[round(t * 2)] = p
    run = 0
    for t, _, p, _ in f30:
        best = None
        for k in range(round(t * 2) - 2 * slack, round(t * 2) + 2 * slack + 1):
            q = at60.get(k)
            if q is not None:
                d = ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5
                best = d if best is None or d < best else best
        if best is None:
            continue
        run = run + 1 if best > apart else 0
        if run >= for_30ths:
            return t - for_30ths
    return None


def compare(f30, f60, min_len):
    """The two runs' actions paired, and what differs: a list of findings (text), by action"""
    cut = parting(f30, f60)
    if cut is not None:
        f30 = [f for f in f30 if f[0] < cut]
        f60 = [f for f in f60 if f[0] < cut]
    s30, s60 = segments(f30), segments(f60)
    w30 = np.array([f[3] for f in f30], dtype=np.uint32)
    w60 = np.array([f[3] for f in f60], dtype=np.uint32)
    sm = difflib.SequenceMatcher(None, [s[0] for s in s30], [s[0] for s in s60], autojunk=False)
    findings = []
    paired = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            continue
        for k in range(i2 - i1):
            act, a0, a1 = s30[i1 + k]
            _, b0, b1 = s60[j1 + k]
            t30 = (f30[a0][0], f30[a1][0] + 1.0)        # (a frame at 30 lasts a 30th)
            t60 = (f60[b0][0], f60[b1][0] + 0.5)
            d30, d60 = t30[1] - t30[0], t60[1] - t60[0]
            if d30 < min_len:
                continue
            paired += 1
            found = []
            last30 = a1 >= len(f30) - 2 or b1 >= len(f60) - 2      # (cut off by a trace's end: its length says nothing)
            if not last30 and abs(d60 - d30) > max(1.5, 0.15 * d30):
                found.append(f"lasts {d60:.1f} 30ths at 60, {d30:.1f} at 30")
            # fields that count game frames at 30 (moving by 1 on most of its frames): as often a 30th at 60?
            n30 = a1 - a0
            c30, c60 = counter_steps(w30, a0, a1), counter_steps(w60, b0, b1)
            for size in (4, 2, 1):
                o30, m30, sp30 = c30[size]
                o60, _, sp60 = c60[size]
                cand = np.nonzero((o30 >= 4) & (o30 >= 0.6 * sp30) & (m30 <= o30 + 2))[0]
                for idx in cand:
                    off = int(idx) * size
                    if off // 4 == ACTION // 4:
                        continue
                    # steps a 30th, over the frames it counted in (a frame a 30th at 30, half one at 60)
                    r30 = o30[idx] / float(sp30[idx])
                    r60 = o60[idx] / (float(sp60[idx]) * 0.5) if o60[idx] else 0.0
                    if r60 > 1.6 * r30 or r60 < 0.6 * r30:
                        found.append(f"+{off:04X} ({size} bytes) steps {r60:.2f} times a 30th at 60, {r30:.2f} at 30")
            l30, l60 = path_length(f30, a0, a1), path_length(f60, b0, b1)
            if not last30 and max(l30, l60) > 50 and not 0.75 < (l60 + 1) / (l30 + 1) < 1.33:
                found.append(f"covers {l60:.0f} units at 60, {l30:.0f} at 30")
            if found:
                findings.append((act, t30, t60, dedupe(found)))
    return paired, findings, cut


def dedupe(found):
    """A counter seen as a word, a half and a byte: the smallest that steps, once"""
    out, seen = [], set()
    for f in sorted(found, key=lambda s: (not s.startswith("+"), s)):
        if f.startswith("+"):
            off = int(f[1:5], 16)
            key = off & ~3
            if key in seen:
                continue
            seen.add(key)
        out.append(f)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recording")
    ap.add_argument("--rate", type=int, choices=(30, 60), default=30, help="the rate it was recorded at")
    ap.add_argument("--saves", default=os.path.expanduser("~/.local/share/mhfu-recomp/SAVEDATA"),
                    help="the saves it was recorded with (copied)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "mhfu"))
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--keep", help="keep the runs (recordings, traces, logs) in this directory")
    ap.add_argument("--reuse", action="store_true", help="with --keep: compare the traces kept there, not run again")
    ap.add_argument("--min", type=float, default=3.0, help="actions shorter than this (30ths, at 30) left out")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    work = args.keep or tempfile.mkdtemp(prefix="ratecheck-")
    os.makedirs(work, exist_ok=True)
    if args.reuse and args.keep:
        return report(args, os.path.join(work, "r30", "trace"), os.path.join(work, "r60", "trace"), work)
    for n in ("r30", "r60"):
        if os.path.exists(os.path.join(work, n)):
            shutil.rmtree(os.path.join(work, n))
    rec30, rec60 = os.path.join(work, "at30.rec"), os.path.join(work, "at60.rec")
    if args.rate == 60:
        shutil.copy(args.recording, rec60)
        convert(args.recording, rec30, 0.5)
    else:
        shutil.copy(args.recording, rec30)
        convert(args.recording, rec60, 2)
    p30, tr30, log30 = run("r30", rec30, 30, args, work)
    p60, tr60, log60 = run("r60", rec60, 60, args, work)
    ok30, ok60 = wait(p30, log30, args.timeout), wait(p60, log60, args.timeout)
    if not (ok30 and ok60):
        print(f"a run did not get to the end of its replay (logs in {work})", file=sys.stderr)
    return report(args, tr30, tr60, work)


def report(args, tr30, tr60, work):
    f30, f60 = load(tr30), load(tr60)
    if not f30 or not f60:
        print(f"no hunter in a hunt in the traces (in the village it is another object; logs in {work})")
        return 2
    paired, findings, cut = compare(f30, f60, args.min)
    n30, n60 = len(segments(f30)), len(segments(f60))
    print(f"{os.path.basename(args.recording)}: {n30} actions at 30, {n60} at 60, {paired} paired and compared")
    if cut is not None:
        def doing(frames):
            at = min(frames, key=lambda f: abs(f[0] - cut))
            return f"{at[3][ACTION // 4]:08X}"
        print(f"  (the runs part from 30th {cut:.0f}, the hunter's action {doing(f30)} at 30, {doing(f60)} at 60 - compared up "
              f"to there; a parting while the mouse turns the camera is the input's conversion, not the game)")
    for act, t30, t60, found in findings:
        print(f"  action {act:08X} at 30ths {t30[0]:.1f}-{t30[1]:.1f} (at 60: {t60[0]:.1f}-{t60[1]:.1f}):")
        for f in found:
            print(f"    {f}")
    if not findings:
        print("  as at 30")
    if not args.keep:
        shutil.rmtree(work)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
