#!/usr/bin/env python3
"""What a MHFU_PRESENT_LOG file says about how evenly the frames reached the screen.

    tools/pacing.py LOG [LOG ...]  [--from SECONDS] [--to SECONDS] [--per-second]

Measured (by us, or reported by the driver): issue, drawn and present-returned times, the driver's time of each frame's
first pixel going out, the game time each frame shows, frames the driver says never reached the screen.
Inferred (worked out here from those): the refresh grid (a straight line fitted through the pixel times), which refresh
each frame took, repeats and dropped refreshes, the content step per refresh, the slide of the issue times against the
refresh, and any periods found in them.
"""
import argparse
import sys

import numpy as np


def load(path):
    frames, ticks, refresh_ns = [], [], None
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "F" and len(p) >= 13:
                frames.append([float(x) for x in p[1:13]])
            elif p[0] == "T" and len(p) >= 4:
                ticks.append((float(p[1]), float(p[2]), float(p[3])))
            elif p[0] == "#" and len(p) >= 3 and p[1] == "refresh_ns":
                refresh_ns = float(p[2])
    # id issue due drawn flip queue pixel content game_frame t ticks busy
    return np.array(frames).reshape(-1, 12), np.array(ticks).reshape(-1, 3), refresh_ns


def spread(x):
    """mean, standard deviation, 1st and 99th percentile, least, most"""
    if len(x) == 0:
        return "(none)"
    a = np.percentile(x, [1, 99])
    return f"mean {x.mean():8.3f}  sd {x.std():6.3f}  p1 {a[0]:8.3f}  p99 {a[1]:8.3f}  min {x.min():8.3f}  max {x.max():8.3f}"


def fit_refresh(pixel, nominal):
    """The refresh, and a grid through the pixel times: each interval as a whole number of refreshes, then a line fitted.
    Returns (refresh us, grid start us, refresh number of each)"""
    r = nominal
    if not r:
        d = np.diff(pixel)
        r = float(np.median(d[d > 1000])) if len(d) else 0
    for _ in range(3):
        n = np.concatenate(([0], np.cumsum(np.maximum(1, np.round(np.diff(pixel) / r)))))
        a, b = np.polyfit(n, pixel, 1)
        r = float(a)
    return r, float(b), n


def peaks(times, values, label, lo=1.5, hi=120.0):
    """The strongest periods in a series on an even grid of 0.25 s, between lo and hi seconds"""
    if len(times) < 10:
        return
    span = times[-1] - times[0]
    if span < 2 * lo:
        return
    dt = 0.25
    bins = np.arange(times[0], times[-1] + dt, dt)
    s, _ = np.histogram(times, bins=bins, weights=values)
    if not s.any():
        print(f"  {label}: nothing to find a period in")
        return
    s = s - s.mean()
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s))))
    freq = np.fft.rfftfreq(len(s), dt)
    keep = (freq > 1 / min(hi, span / 2)) & (freq < 1 / lo)
    if not keep.any():
        return
    order = np.argsort(spec[keep])[::-1][:3]
    med = np.median(spec[keep]) or 1
    txt = ", ".join(f"{1 / freq[keep][i]:.1f} s (x{spec[keep][i] / med:.0f})" for i in order)
    print(f"  {label}: strongest periods {txt} [x: over the median of the spectrum; below about x5 is noise]")


def report(path, args):
    F, T, nominal_ns = load(path)
    if len(F) == 0:
        print(f"{path}: no frames")
        return
    issue = F[:, 1]
    keep = np.ones(len(F), bool)
    if args.skip is not None:
        keep &= issue >= issue[0] + args.skip * 1e6
    if args.to is not None:
        keep &= issue <= issue[0] + args.to * 1e6
    F = F[keep]
    if len(F) < 10:
        print(f"{path}: too few frames in the range")
        return
    id_, issue, due, drawn, flip, queue, pixel, content, game_frame, tt, ticks, busy = F.T
    nominal = nominal_ns / 1000 if nominal_ns else None
    secs = (issue[-1] - issue[0]) / 1e6
    print(f"== {path}: {len(F)} frames over {secs:.1f} s")

    print("MEASURED, our clock:")
    d = np.diff(issue) / 1000
    print(f"  issue interval ms          {spread(d)}")
    late = -due / 1000
    print(f"  issue late of its due, ms  {spread(late)}")
    print(f"  issue to queued draws, ms  {spread(drawn / 1000)}")
    print(f"  issue to present back, ms  {spread(flip / 1000)}")
    tick_before = ticks > 0
    if tick_before.any() and (~tick_before).any():
        print(f"  issue late, tick before    {spread(late[tick_before])}")
        print(f"  issue late, none before    {spread(late[~tick_before])}")
        print(f"  frames with a tick before: {tick_before.mean() * 100:.1f}%; busy before the frame, ms: {spread(busy / 1000)}")
    gd = np.diff(T[:, 0]) / 1000 if len(T) > 1 else np.array([])
    if len(gd):
        print(f"  tick interval ms (all)     {spread(gd)}")

    have = pixel > 0
    gone = pixel < 0
    print("MEASURED, the driver's reports:")
    print(f"  frames: shown {int(have.sum())}, never shown {int(gone.sum())}, no report {int((pixel == 0).sum())}")
    if not have.any():
        print("  (no presentation times in this log: everything below needs them)")
        return
    t_px = (issue + pixel)[have]
    order_ok = np.all(np.diff(t_px) >= 0)
    if not order_ok:
        print("  (pixel times go back somewhere)")
    pi = np.diff(t_px) / 1000
    print(f"  shown interval ms          {spread(pi)}")
    print(f"  issue to first pixel, ms   {spread((pixel[have]) / 1000)}")
    q = queue > 0
    if q.any():
        print(f"  issue to queue end, ms     {spread(queue[q] / 1000)}")

    print("INFERRED, the refresh and the grid:")
    r, start, n = fit_refresh(t_px, nominal)
    resid = (t_px - (start + r * n))
    print(f"  refresh fitted {r:.3f} us ({1e6 / r:.5f} Hz){'; driver says ' + format(nominal, '.3f') + ' us' if nominal else ''}; "
          f"pixel times off the line: sd {resid.std():.1f} us, worst {np.abs(resid).max():.1f} us")
    mean_issue = np.mean(d) if len(d) else 0
    if mean_issue > 0:
        beat = abs(1 / (1 / r - 1 / (mean_issue * 1000)))
        if beat / 1e6 < 36000:
            print(f"  issue rate {1e3 / mean_issue:.4f}/s against that: phase slides round every {beat / 1e6:.1f} s")
    steps = np.diff(n).astype(int)
    refreshes = int(n[-1]) + 1
    repeated = int(np.maximum(0, steps - 1).sum())
    print(f"  refreshes over the run {refreshes}: repeats (a refresh with no new frame) {repeated} "
          f"({repeated / secs * 60:.1f} a minute)")
    print(f"  never shown (replaced) {int(gone.sum())} ({gone.sum() / secs * 60:.1f} a minute)")
    if n[-1] != len(t_px) - 1:
        print(f"  frames shown per refresh {len(t_px) / refreshes:.4f}")

    # the eye: the game's time from one refresh to the next
    c = content[have]
    dc = np.diff(c) / 1000
    per = dc / np.maximum(steps, 1)
    ideal = r / 1000
    print("INFERRED, what the eye sees (the game time shown, by refresh):")
    print(f"  ideal step {ideal:.3f} ms a refresh")
    # by refresh: repeats are a step of 0 then the rest
    all_steps = []
    for s, v in zip(steps, dc):
        all_steps += [0.0] * (s - 1)
        all_steps.append(float(v))
    all_steps = np.array(all_steps)
    print(f"  step per refresh, ms       {spread(all_steps)}")
    off = np.abs(all_steps - ideal)
    print(f"  steps off by over 10%: {(off > ideal * 0.1).sum()} of {len(all_steps)} ({(off > ideal * 0.1).mean() * 100:.3f}%), "
          f"over 50%: {(off > ideal * 0.5).sum()} ({(off > ideal * 0.5).mean() * 100:.3f}%)")
    print(f"  content behind the screen, ms (pixel time less game time shown; a change is a jerk): "
          f"{spread(((t_px - c) - (t_px - c)[0]) / 1000)}")
    held = (tt >= 9999)[have]
    if held.any():
        print(f"  frames at t = 1 (no newer game frame to go to): {held.sum()} ({held.mean() * 100:.2f}%)")

    # when: uneven refreshes, in 5 s bins, against the phase of the issue on the refresh
    secs_px = (t_px - t_px[0]) / 1e6
    bad = np.abs(np.diff(c) / 1000 / 1 - ideal * steps) > ideal * 0.5
    bad |= steps > 1
    bad_at = secs_px[1:][bad]
    print("INFERRED, when:")
    if secs > 12:
        print(f"  uneven refreshes in 5 s bins: {' '.join(str(int(x)) for x in np.histogram(bad_at, bins=np.arange(0, secs + 5, 5))[0])}")
    peaks(secs_px[1:], bad.astype(float), "uneven refreshes")
    gone_at = (issue[gone] - issue[0]) / 1e6
    if len(gone_at):
        peaks(gone_at, np.ones(len(gone_at)), "replaced frames")

    # the frame's way to the screen: the phase on the grid when it was queued, and whether it lost a refresh
    phase = np.mod((issue + flip)[have] - start, r) / r
    if steps.size and repeated:
        h_all, _ = np.histogram(phase[1:], bins=10, range=(0, 1))
        h_bad, _ = np.histogram(phase[1:][steps > 1], bins=10, range=(0, 1))
        print("  present returned, phase on the refresh (0-1, tenths), all frames / with a repeat before:")
        print("   ", " ".join(f"{a}/{b}" for a, b in zip(h_all, h_bad)))
    if args.per_second:
        print("  second: shown repeats")
        for s in range(int(secs_px[-1]) + 1):
            m = (secs_px[1:] >= s) & (secs_px[1:] < s + 1)
            print(f"  {s:4d}: {m.sum():4d} {int(np.maximum(0, steps[m] - 1).sum())}")

    if tick_before.any():
        tb = tick_before[have][1:]
        if tb.any() and (~tb).any():
            print(f"  repeats by whether a tick came just before the frame: with {(steps[tb] > 1).mean() * 100:.3f}% "
                  f"of {tb.sum()}, without {(steps[~tb] > 1).mean() * 100:.3f}% of {(~tb).sum()}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="+")
    ap.add_argument("--from", dest="skip", type=float, help="leave out the first seconds")
    ap.add_argument("--to", type=float, help="to this many seconds from the start")
    ap.add_argument("--per-second", action="store_true")
    args = ap.parse_args()
    for path in args.log:
        report(path, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
