#!/usr/bin/env python3
"""The DLLs the Windows build needs, copied beside it (docs/windows.md), found by what it and they import:

    python3 tools/win_dlls.py build/win/mhfu.exe DIR...      (DIR: where to look for them; llvm-readobj on the PATH)

vulkan-1.dll is left out: it comes with the graphics driver."""
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    exe, dirs = sys.argv[1], sys.argv[2:]
    files = {}
    for d in reversed(dirs):
        files.update({f.lower(): os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".dll")})
    seen, todo = {"vulkan-1.dll"}, [exe]
    copied = []
    while todo:
        out = subprocess.run(["llvm-readobj", "--coff-imports", todo.pop()], capture_output=True, text=True,
                             check=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("Name: ") and line.lower().endswith(".dll") and line[6:].lower() not in seen:
                name = line[6:].lower()
                seen.add(name)
                if name in files:       # (else Windows' own)
                    copied.append(files[name])
                    todo.append(files[name])
    for path in copied:
        shutil.copy2(path, os.path.dirname(os.path.abspath(exe)))
    print(f"{len(copied)} DLLs beside {exe}, {sum(os.path.getsize(p) for p in copied) // 2**20} MB")


if __name__ == "__main__":
    main()
