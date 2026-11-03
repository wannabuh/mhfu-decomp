#!/usr/bin/env python3
"""The test scenarios (tests/scenarios.toml) played and checked: a recording each, cut and extended as the scenario
says (input lines, pokes), headless.

Checks, as each scenario asks:

  rate      played at 30 and at 60 and compared action by action (tools/ratecheck.py): what lasts longer, counts
            twice as fast or covers more ground at 60;
  baseline  the game's RAM hashed every 10 seconds of game time, and the hunter's actions in order, at 30 and at 60,
            against those kept (--bless keeps this run's): where a run first changed;
  state     a save state at 60 saved and loaded (MHFU_STATE_TEST): every frame the same after the load.

What the hunter did in all of them (the states, +0x299, by weapon class +0x1E8) is the coverage: the states seen, and
those of the game's table never seen. The data (recordings, saves, baselines, runs) is in tests/data/, not in the
repository (the game's and the player's).

    tools/suite.py [-j 6] [--only name,tag,...] [--bless] [--keep] [--list]

Exits 1 if anything failed or changed. A report goes to tests/data/runs/<date>/report.md.
"""
import argparse
import concurrent.futures as cf
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ratecheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
DATA = os.path.join(TESTS, "data")
STATE_BYTE = 0x299                  # the hunter's state: game_task 09A9A9E0 switches on it (0x80 and up: the weapon's)
CLASS_BYTE = 0x1E8                  # the weapon's class (0-10)
HASH_30THS = 300                    # the RAM hashed every so many 30ths


# -- scenarios -------------------------------------------------------------------------------------------------------

def load_scenarios():
    with open(os.path.join(TESTS, "scenarios.toml"), "rb") as f:
        doc = tomllib.load(f)
    defaults = doc.get("defaults", {})
    out = []
    for s in doc.get("scenario", []):
        d = dict(defaults)
        d.update(s)
        d.setdefault("checks", ["rate", "baseline"])
        d.setdefault("tags", [])
        d.setdefault("rate", 30)
        d.setdefault("switch", 0)
        out.append(d)
    return out


def build_recording(s, path):
    """The scenario's recording: its base, cut at `cut` (input after it and the end dropped), its lines added"""
    src = os.path.join(DATA, "rec", s["rec"])
    cut = s.get("cut")
    lines = []
    for line in open(src):
        p = line.split()
        if cut is not None and p:
            if p[0] == "end":
                continue
            if p[0].isdigit() and int(p[0]) > cut:
                continue
            if p[0] in ("pointer", "poke") and int(p[1].split("-")[0]) > cut:
                continue
        lines.append(line.rstrip("\n"))
    for line in s.get("lines", "").strip().splitlines():
        if line.strip() and not line.strip().startswith("#"):
            lines.append(line.strip())
    with open(path, "w") as o:
        o.write("\n".join(lines) + "\n")


def end_30ths(path, rate, switch):
    """A recording's end in game time (30ths): at 60, its frames after the switch count half"""
    e = end_frame(path)
    if rate == 30:
        return e
    return switch + (e - switch) / 2 if switch else e / 2


def end_frame(path):
    """A recording's end: its "end" line, or (none) its last input's frame, as the runtime takes it"""
    end = last = 0
    for line in open(path):
        p = line.split()
        if p and p[0] == "end":
            end = int(p[1])
        elif p and p[0].isdigit():
            last = max(last, int(p[0]))
    return end or last


# -- runs ------------------------------------------------------------------------------------------------------------

def start_run(work, name, rec, rate, s, args, trace=True, extra=None):
    d = os.path.join(work, name)
    if os.path.exists(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "build"))
    os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
    shutil.copytree(os.path.join(DATA, "saves", s["saves"]), os.path.join(d, "SAVEDATA"))
    os.makedirs(os.path.join(d, "cfg", "mhfu-recomp"))
    with open(os.path.join(d, "cfg", "mhfu-recomp", "settings.ini"), "w") as f:
        f.write(f"iso = {args.iso}\n")
    sw = s["switch"]
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_FPS="0",
               MHFU_RENDERER="soft", MHFU_SCALE="1",
               MHFU_TICK_HZ=f"{rate}@{sw}" if sw and rate != 30 else str(rate),
               MHFU_REPLAY=os.path.abspath(rec), MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"),
               MHFU_STATEDIR=os.path.join(d, "states"),
               MHFU_RAM_HASH=str(HASH_30THS * rate // 30))
    for k in [k for k in env if k.startswith("MHFU_TEST_") or k in ("MHFU_REPLAY_SPEED", "MHFU_RECORD")]:
        del env[k]
    if trace:
        env.update(MHFU_NATIVE_TRACE=os.path.join(d, "trace"), MHFU_TRACE_WORDS=f"0,{ratecheck.WORDS:x}",
                   MHFU_TRACE_VT="089bb3cc")
    env.update(extra or {})
    log = open(os.path.join(d, "log"), "w")
    p = subprocess.Popen([os.path.abspath(args.bin)], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
    return p, d


def wait_run(p, d, timeout, until="replay done"):
    """Until the log says `until` (the game plays on after a replay: stopped), the process ends, or the timeout"""
    t0 = time.time()
    path = os.path.join(d, "log")
    done = False
    while time.time() - t0 < timeout:
        with open(path, errors="replace") as f:
            if until in f.read():
                done = True
                break
        if p.poll() is not None:
            break
        time.sleep(1)
    if p.poll() is None:
        p.kill()
    p.wait()
    return done


def ram_hashes(d):
    out = []
    for line in open(os.path.join(d, "log"), errors="replace"):
        m = re.match(r"ram hash (\d+) ([0-9a-f]+)", line)
        if m:
            out.append((int(m.group(1)), m.group(2)))
    return out


def crashed(d):
    text = open(os.path.join(d, "log"), errors="replace").read()
    for key in ("bad jump", "unreachable", "Segmentation", "Aborted", "assert", "unknown syscall", "not translated"):
        if key in text:
            return key
    return None


# -- what a trace says -----------------------------------------------------------------------------------------------

def state_of(w):
    return (w[STATE_BYTE // 4] >> (8 * (STATE_BYTE % 4))) & 0xFF


def class_of(w):
    return (w[CLASS_BYTE // 4] >> (8 * (CLASS_BYTE % 4))) & 0xFF


def state_runs(frames):
    """The hunter's states in order with how long each lasted (30ths): [(class, state, 30ths)]"""
    out = []
    for t, _, _, w in frames:
        k = (class_of(w), state_of(w))
        if out and out[-1][0] == k:
            out[-1][1][1] = t
        else:
            out.append((k, [t, t]))
    return [(k[0], k[1], round(b - a, 1)) for k, (a, b) in out]


def action_of(w):
    return w[ACTION_WORD] & 0xFFFF


ACTION_WORD = 0x684 // 4            # the hunter's action (+0x684, u16: the weapon's moves are their own numbers)


def coverage(frames):
    """{"class:state": frames, "class:a<action>": frames}"""
    seen = {}
    for _, _, _, w in frames:
        for k in (f"{class_of(w)}:{state_of(w):02X}", f"{class_of(w)}:a{action_of(w):04X}"):
            seen[k] = seen.get(k, 0) + 1
    return seen


# -- one scenario ----------------------------------------------------------------------------------------------------

def run_scenario(s, args, stamp):
    name = s["name"]
    work = os.path.join(DATA, "runs", stamp, name)
    os.makedirs(work, exist_ok=True)
    res = {"name": name, "tags": s["tags"], "checks": {}, "coverage": {}, "notes": []}
    t0 = time.time()
    rec = os.path.join(work, "scenario.rec")
    build_recording(s, rec)
    rec30, rec60 = os.path.join(work, "at30.rec"), os.path.join(work, "at60.rec")
    if s["rate"] == 60:
        shutil.copy(rec, rec60)
        ratecheck.convert(rec, rec30, 0.5)
    else:
        shutil.copy(rec, rec30)
        ratecheck.convert(rec, rec60, 2, s["switch"])
    checks = s["checks"]
    runs = {}
    rates = (30, 60) if ("rate" in checks or "baseline" in checks) else ()
    for r in rates:
        runs[r] = start_run(work, f"r{r}", rec30 if r == 30 else rec60, r, s, args)
    state_run = None
    if "state" in checks:
        e = end_frame(rec60)
        a, b = s.get("state_frames", [e - 400, e - 100])
        state_run = start_run(work, "state", rec60, 60, s, args, trace=False,
                              extra={"MHFU_STATE_TEST": f"{a},{b}"})
    frames = {}
    for r, (p, d) in runs.items():
        ok = wait_run(p, d, args.timeout)
        bad = crashed(d)
        if bad:
            res["notes"].append(f"run at {r}: '{bad}' in its log")
        if not ok:
            res["notes"].append(f"run at {r} did not reach its replay's end")
        tr = os.path.join(d, "trace")
        frames[r] = ratecheck.load(tr) if os.path.exists(tr) else []
        # (what follows the replay's end is the game left alone until the run is stopped - when, varies: left out)
        end = end_30ths(rec30 if r == 30 else rec60, r, s["switch"])
        frames[r] = [f for f in frames[r] if f[0] < end - 1]
        res["coverage"][r] = coverage(frames[r])
    if state_run:
        p, d = state_run
        wait_run(p, d, args.timeout, until="again:")
        text = open(os.path.join(d, "log"), errors="replace").read()
        lines = [ln for ln in text.splitlines() if ln.startswith("state test")]
        ok = any("RAM the same" in ln for ln in lines) and not any("first differs" in ln for ln in lines)
        res["checks"]["state"] = {"ok": ok, "detail": lines[-3:] if lines else ["no state test ran (frames?)"]}
    if "rate" in checks:
        f30, f60 = frames.get(30, []), frames.get(60, [])
        if s["switch"]:
            f30 = [f for f in f30 if f[0] >= s["switch"] + 1]
            f60 = [f for f in f60 if f[0] >= s["switch"] + 1]
        if "until" in s:                # (compared up to a 30th: where the input's conversion is known to part them)
            f30 = [f for f in f30 if f[0] < s["until"]]
            f60 = [f for f in f60 if f[0] < s["until"]]
        if not f30 or not f60:
            res["checks"]["rate"] = {"ok": False, "detail": ["no hunter in a hunt in a trace"]}
        else:
            ratecheck.WINDOW = 0
            paired, findings, cut = ratecheck.compare(f30, f60, s.get("min", 3.0), s.get("whole", False))
            detail = [f"{paired} actions paired"]
            if cut is not None:
                detail.append(f"the runs part from 30th {cut:.0f}")
            for act, t30, t60, found in findings:
                detail.append(f"action {act:08X} at 30ths {t30[0]:.1f}-{t30[1]:.1f}: " + "; ".join(found))
            allowed = s.get("allow", [])
            real = [f for f in findings if not any(a in "; ".join(f[3]) for a in allowed)]
            res["checks"]["rate"] = {"ok": not real, "detail": detail}
    if "baseline" in checks:
        now = {}
        for r in rates:
            last = end_frame(rec30 if r == 30 else rec60)
            now[str(r)] = {"hashes": [h for h in ram_hashes(runs[r][1]) if h[0] < last],
                           "states": state_runs(frames[r])}
        res["baseline_now"] = now
        path = os.path.join(DATA, "baselines", f"{name}.json")
        if args.bless or not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                json.dump(now, f)
            res["checks"]["baseline"] = {"ok": True, "detail": ["kept as the baseline" +
                                                                 (" (--bless)" if args.bless else " (none before)")]}
        else:
            was = json.load(open(path))
            detail, ok = [], True
            for r in now:
                if r not in was:
                    continue
                hn = dict(map(tuple, now[r]["hashes"]))
                hw = dict(map(tuple, was[r]["hashes"]))
                common = sorted(set(hn) & set(hw))
                first = next((f for f in common if hn[f] != hw[f]), None)
                if first is not None:
                    ok = False
                    last_same = max([f for f in common if f < first], default=0)
                    sn, sw = now[r]["states"], [tuple(x) for x in was[r]["states"]]
                    k = next((i for i, (a, b) in enumerate(zip(sn, sw)) if tuple(a) != tuple(b)), None)
                    where = ""
                    if k is not None:
                        where = f"; the hunter's states first differ at #{k}: {fmt_run(sw[k])} before, {fmt_run(sn[k])} now"
                    detail.append(f"at {r}: RAM differs from frame {first} (the same at {last_same}){where}")
                elif len(hn) != len(hw):
                    detail.append(f"at {r}: {len(hn)} hashes now, {len(hw)} before (the same where both)")
                else:
                    detail.append(f"at {r}: the same ({len(common)} hashes)")
            res["checks"]["baseline"] = {"ok": ok, "detail": detail}
    res["seconds"] = round(time.time() - t0)
    if not args.keep:
        for r in ("r30", "r60"):
            tr = os.path.join(work, r, "trace")
            if os.path.exists(tr):
                os.remove(tr)
    return res


def fmt_run(r):
    return f"class {r[0]} state {r[1]:02X} for {r[2]} 30ths"


# -- the game's table of the hunter's states -------------------------------------------------------------------------

def state_table():
    """The hunter's states (game_task 09A9AA0C's switch on +0x299, below 0x80): {state: handler} - the function each
    case calls, with its argument"""
    path = os.path.join(ROOT, "build", "gen", "game_task_0.c")
    if not os.path.exists(path):
        return {}
    text = open(path).read()
    m = re.search(r"/\* 09A9AA0C \*/ \{ uint32_t t = c->r\[2\];  switch \(t\) \{(.*?)default", text)
    if not m:
        return {}
    labels = re.findall(r"case 0x([0-9A-F]+)u: goto (L_[0-9A-F]+);", m.group(1))
    out = {}
    for i, (_, label) in enumerate(labels):
        body = re.search(re.escape(label) + r":;\n(.*?)\n(?:L_|\})", text, re.S)
        call = re.search(r"(?:c->r\[5\] = 0 \+ (0x[0-9A-F]+u|0);)?\s*f_(game_task_[0-9A-F]+)\(c\)", body.group(1)) if body else None
        if call:
            arg = call.group(1)
            out[i] = call.group(2).replace("game_task_", "") + (f"({int(arg.rstrip('u'), 0)})" if arg else "")
    return out


# -- the report ------------------------------------------------------------------------------------------------------

def report(results, stamp, args):
    lines = [f"# Test suite, {stamp}", ""]
    failed = 0
    lines.append("| scenario | " + " | ".join(("rate", "baseline", "state")) + " | time |")
    lines.append("|---|---|---|---|---|")
    for r in results:
        cells = []
        for c in ("rate", "baseline", "state"):
            v = r["checks"].get(c)
            cells.append("-" if v is None else ("ok" if v["ok"] else "**FAIL**"))
            if v is not None and not v["ok"]:
                failed += 1
        if r["notes"]:
            failed += 1
        lines.append(f"| {r['name']} | " + " | ".join(cells) + f" | {r['seconds']} s |")
    lines.append("")
    for r in results:
        lines.append(f"## {r['name']}")
        for n in r["notes"]:
            lines.append(f"- note: {n}")
        for c, v in r["checks"].items():
            lines.append(f"- {c}: {'ok' if v['ok'] else 'FAIL'}")
            for d in v["detail"]:
                lines.append(f"  - {d}")
        lines.append("")
    # coverage
    seen = {}
    for r in results:
        for rate, cov in r["coverage"].items():
            for k, n in cov.items():
                seen.setdefault(k, set()).add(r["name"])
    table = state_table()
    lines.append("## Coverage: the hunter's states seen")
    by_class, actions = {}, {}
    for k in seen:
        cl, st = k.split(":")
        if st.startswith("a"):
            actions.setdefault(int(cl), set()).add(int(st[1:], 16))
        else:
            by_class.setdefault(int(cl), set()).add(int(st, 16))
    for cl in sorted(set(by_class) | set(actions)):
        sts = sorted(by_class.get(cl, ()))
        acts = sorted(actions.get(cl, ()))
        lines.append(f"- weapon class {cl}: {len(sts)} states: " + ", ".join(f"{x:02X}" for x in sts))
        lines.append(f"  - {len(acts)} actions (+0x684): " + ", ".join(f"{x:04X}" for x in acts))
    common = set().union(*by_class.values()) if by_class else set()
    if table:
        handlers_seen = {table[s] for s in common if s in table}
        never = sorted({h for h in table.values()} - handlers_seen)
        lines.append(f"- the common states (below 0x80): {len(handlers_seen)} of {len(set(table.values()))} handlers "
                     "seen; never seen:")
        inv = {}
        for st, h in table.items():
            inv.setdefault(h, []).append(st)
        for h in never:
            lines.append(f"  - {h}: state " + ", ".join(f"{x:02X}" for x in inv[h]))
    out = os.path.join(DATA, "runs", stamp, "report.md")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(DATA, "runs", stamp, "report.json"), "w") as f:
        json.dump([{k: v for k, v in r.items() if k != "baseline_now"} for r in results], f, indent=1)
    return failed, out, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-j", type=int, default=4, help="scenarios at once (each runs up to three games)")
    ap.add_argument("--only", help="names or tags, comma-separated")
    ap.add_argument("--bless", action="store_true", help="keep this run's results as the baselines")
    ap.add_argument("--keep", action="store_true", help="keep the traces")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "mhfu"))
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--timeout", type=int, default=1200)
    args = ap.parse_args()
    scen = load_scenarios()
    if args.only:
        want = set(args.only.split(","))
        scen = [s for s in scen if s["name"] in want or want & set(s["tags"])]
    if args.list:
        for s in scen:
            print(f"{s['name']:24} {','.join(s['checks']):22} {','.join(s['tags'])}")
        return 0
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")
    os.makedirs(os.path.join(DATA, "runs", stamp))
    results = []
    with cf.ThreadPoolExecutor(max_workers=args.j) as ex:
        futs = {ex.submit(run_scenario, s, args, stamp): s for s in scen}
        for fu in cf.as_completed(futs):
            s = futs[fu]
            try:
                r = fu.result()
            except Exception as e:      # (a scenario's own error: reported, the rest go on)
                r = {"name": s["name"], "tags": s["tags"], "checks": {}, "coverage": {}, "seconds": 0,
                     "notes": [f"error: {e!r}"]}
            results.append(r)
            marks = " ".join(f"{c}:{'ok' if v['ok'] else 'FAIL'}" for c, v in r["checks"].items())
            print(f"{r['name']:24} {marks} {'; '.join(r['notes'])} ({r['seconds']} s)", flush=True)
    order = {s["name"]: i for i, s in enumerate(scen)}
    results.sort(key=lambda r: order[r["name"]])
    failed, out, _ = report(results, stamp, args)
    print(f"report: {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
