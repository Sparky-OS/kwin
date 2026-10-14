#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""mkcpio.py TREE OUT.cpio.gz : pack a directory tree as a gzip-compressed 'newc' cpio archive for use as an initramfs.

Owned by root, with /dev/console (5,1) and /dev/null (1,3) character nodes added, which a normal user cannot create
with mknod. Symbolic links are kept. Needs nothing but Python 3."""
import gzip, os, stat, sys

def main():
    tree, out = sys.argv[1], sys.argv[2]
    entries = []   # (name, mode, data, rdev)
    nodes = {"dev/console": (stat.S_IFCHR | 0o600, (5, 1)), "dev/null": (stat.S_IFCHR | 0o666, (1, 3))}
    for root, dirs, files in os.walk(tree):
        dirs.sort()
        rel = os.path.relpath(root, tree)
        if rel != ".":
            entries.append((rel, stat.S_IFDIR | 0o755, b"", (0, 0)))
        for f in sorted(files):
            p = os.path.join(root, f)
            r = os.path.relpath(p, tree)
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                entries.append((r, stat.S_IFLNK | 0o777, os.readlink(p).encode(), (0, 0)))
            else:
                entries.append((r, stat.S_IFREG | (st.st_mode & 0o777), open(p, "rb").read(), (0, 0)))
    have = {e[0] for e in entries}
    if "dev" not in have:
        entries.append(("dev", stat.S_IFDIR | 0o755, b"", (0, 0)))
    for n, (mode, rdev) in nodes.items():
        entries.append((n, mode, b"", rdev))
    entries.sort(key=lambda e: e[0])
    buf = bytearray()
    ino = 1
    def pad(n): return (4 - n % 4) % 4
    for name, mode, data, rdev in entries + [("TRAILER!!!", 0, b"", (0, 0))]:
        nm = name.encode() + b"\0"
        hdr = "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x" % (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(nm), 0)
        buf += hdr.encode() + nm
        buf += b"\0" * pad(110 + len(nm))
        buf += data
        buf += b"\0" * pad(len(data))
        ino += 1
    with gzip.GzipFile(out, "wb", mtime=0) as g:
        g.write(bytes(buf))

main()
