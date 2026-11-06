#!/usr/bin/env python3
"""The tools to cross-compile the Windows build from Linux, into a folder of their own (docs/windows.md):

    python3 tools/win_setup.py ~/mhfu-wintools

llvm-mingw (the compiler, checked against the hash below) and MSYS2's clang64 packages of the libraries (each checked against
the hash in MSYS2's package list). No root, nothing installed elsewhere: delete the folder to remove them all. About 275 MB
to fetch, 2 GB unpacked."""
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib.request

LLVM_MINGW = "llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/{LLVM_MINGW}.tar.xz"
LLVM_MINGW_SHA256 = "919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926"
MSYS2 = "https://repo.msys2.org/mingw/clang64/"
PREFIX = "mingw-w64-clang-x86_64-"
WANTED = ["sdl3", "libpng", "zlib", "xxhash", "vulkan-loader", "vulkan-headers", "ffmpeg"]
# What llvm-mingw has already
SUPPLIED = re.compile(PREFIX + r"(crt-git|headers-git|winpthreads|libwinpthread|gcc|clang|llvm|lld|compiler-rt|libc\+\+|"
                      r"libunwind|libmangle|tools-git)")


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "mhfu-win-setup"})
    with urllib.request.urlopen(request, timeout=600) as r:
        return r.read()


def fetched(path, url, sha256):
    data = open(path, "rb").read() if os.path.exists(path) else fetch(url)
    if hashlib.sha256(data).hexdigest() != sha256:
        sys.exit(f"{url}: not the file expected (its hash differs)")
    with open(path, "wb") as f:
        f.write(data)


def packages():
    db = tarfile.open(fileobj=io.BytesIO(fetch(MSYS2 + "clang64.db")))
    found, provides = {}, {}
    for member in db.getmembers():
        if member.name.endswith("/desc"):
            text = db.extractfile(member).read().decode()
            fields = {k: v.strip().split("\n") for k, v in re.findall(r"%([A-Z0-9]+)%\n(.*?)(?:\n\n|\Z)", text, re.S)}
            found[fields["NAME"][0]] = fields
            for p in fields.get("PROVIDES", []):
                provides[re.split("[<>=]", p)[0]] = fields["NAME"][0]
    todo, seen = [PREFIX + w for w in WANTED], []
    while todo:
        name = re.split("[<>=]", todo.pop())[0]
        name = name if name in found else provides.get(name, name)
        if name in seen:
            continue
        if name not in found:
            sys.exit(f"{name}: not in MSYS2's package list")
        seen.append(name)
        todo += found[name].get("DEPENDS", [])
    return [found[n] for n in seen if not SUPPLIED.match(n)]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.abspath(os.path.expanduser(sys.argv[1]))
    downloads, sysroot = os.path.join(root, "dl"), os.path.join(root, "sysroot")
    os.makedirs(downloads, exist_ok=True)
    os.makedirs(sysroot, exist_ok=True)
    archive = os.path.join(downloads, LLVM_MINGW + ".tar.xz")
    print("the compiler...", flush=True)
    fetched(archive, LLVM_MINGW_URL, LLVM_MINGW_SHA256)
    if not os.path.isdir(os.path.join(root, "llvm-mingw")):
        subprocess.run(["tar", "xf", archive, "-C", root], check=True)
        os.rename(os.path.join(root, LLVM_MINGW), os.path.join(root, "llvm-mingw"))
    wanted = packages()
    print(f"{len(wanted)} packages of libraries...", flush=True)
    for fields in wanted:
        name = fields["FILENAME"][0]
        path = os.path.join(downloads, name)
        fetched(path, MSYS2 + name, fields["SHA256SUM"][0])
        listing = subprocess.run(["tar", "tf", path], check=True, capture_output=True, text=True).stdout
        if any(line.startswith("clang64/") for line in listing.splitlines()):   # (some only name other packages)
            subprocess.run(["tar", "xf", path, "-C", sysroot, "--strip-components=1", "clang64/"], check=True)
    print(f"ready: PATH={root}/llvm-mingw/bin:$PATH make WINDOWS=1 WINROOT={sysroot}")


if __name__ == "__main__":
    main()
