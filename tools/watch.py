#!/usr/bin/env python3
"""Who writes (or reads) a game variable: run the game under gdb, and from a game frame on stop at each access to a
PSP address (a hardware watchpoint on the word), printing the game functions on the stack and the instruction.

    tools/watch.py 09998F20 --frame 3700 [--read|--access] [--hits 20] [-- MHFU_REPLAY=quest.rec ...]

Environment for the game goes after "--" (or is inherited). Headless and mute unless set. A replay (MHFU_REPLAY) is
the way to get somewhere: the game's run is the same every time.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

GDB_SCRIPT = r'''
import gdb, re
ADDR = {addr}
HITS = {hits}
KIND = {kind}
lines = {{}}

def guest_pc(filename, line):
    """The PSP address of the instruction at a line of generated C (its /* XXXXXXXX */ comment)"""
    try:
        if filename not in lines:
            lines[filename] = open(filename).read().split("\n")
        m = re.search(r"/\* ([0-9A-F]{{8}}) \*/", lines[filename][line - 1])
        return m.group(1) if m else "?"
    except Exception:
        return "?"

class Watch(gdb.Breakpoint):
    hits = 0
    def stop(self):
        Watch.hits += 1
        f = gdb.newest_frame()
        out = []
        while f is not None and len(out) < 12:
            name = f.name() or "?"
            sal = f.find_sal()
            if name.startswith("f_"):
                pc = guest_pc(sal.symtab.fullname(), sal.line) if sal.symtab else "?"
                out.append("%s@%s" % (name[2:], pc))
            elif not out and name not in ("rt_call", "thread_main"):
                out.append(name)
            f = f.older()
        word = int.from_bytes(gdb.selected_inferior().read_memory(HOST, 4).tobytes(), "little")
        print("hit %d: %08X  %s" % (Watch.hits, word, "  <-  ".join(out)), flush=True)
        return Watch.hits >= HITS

gdb.Breakpoint("debug_frame_hook")
gdb.execute("run")
HOST = int(gdb.parse_and_eval("rt_mem")) + (ADDR & 0x3FFFFFFF)
print("watching %08X from frame %s" % (ADDR, gdb.parse_and_eval("n")), flush=True)
Watch("*(int*)%d" % HOST, gdb.BP_WATCHPOINT, KIND)
try:
    gdb.execute("continue")
finally:
    gdb.execute("kill")
'''


def main():
    argv = sys.argv[1:]
    env_args = []
    if "--" in argv:
        k = argv.index("--")
        argv, env_args = argv[:k], argv[k + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("addr", help="PSP address (hex)")
    ap.add_argument("--frame", type=int, required=True, help="game frame to start watching at")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--read", action="store_true")
    g.add_argument("--access", action="store_true")
    ap.add_argument("--hits", type=int, default=20)
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args(argv)
    env = dict(os.environ)
    env.setdefault("MHFU_HEADLESS", "1")
    env.setdefault("MHFU_MUTE", "1")
    for e in env_args:
        k, _, v = e.partition("=")
        env[k] = v
    env["MHFU_DEBUG_FRAME"] = str(args.frame)
    kind = "r" if args.read else "a" if args.access else "w"
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
        f.write(GDB_SCRIPT.format(addr=int(args.addr, 16), hits=args.hits,
                                  kind={"w": "gdb.WP_WRITE", "r": "gdb.WP_READ", "a": "gdb.WP_ACCESS"}[kind]))
        script = f.name
    try:
        p = subprocess.run(["gdb", "-q", "-batch", "-ex", "set pagination off", "-ex", "set print thread-events off",
                            "-ex", "set debuginfod enabled off", "-ex", "handle SIGPIPE nostop noprint",
                            "-x", script, "--args", "build/mhfu", args.iso],
                           env=env, capture_output=True, text=True, timeout=args.timeout)
        out = p.stdout
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
    finally:
        os.unlink(script)
    for line in out.splitlines():
        if line.startswith(("hit ", "watching ")):
            print(line)


if __name__ == "__main__":
    main()
