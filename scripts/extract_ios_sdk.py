#!/usr/bin/env python3
"""Extract iPhoneOS.sdk from a decoded Xcode payload (odc cpio on stdin) and make its TBDs usable.

Xcode stores files shared between SDKs as hardlinks: one entry per inode carries the data and the
others hold "NULLcanary" filler of the same size, so a plain cpio leaves the filler in place. Data
is resolved by inode here, and the SDK must come out with no filler left.
"""
import os
import re
import shutil
import sys

CANARY = b"NULLcanary"
SDK_DIR = "/Platforms/iPhoneOS.platform/Developer/SDKs/"
ANY_SDK = ".platform/Developer/SDKs/"
S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0o170000, 0o100000, 0o040000, 0o120000


def copy(src, n, dst):
    while n > 0:
        chunk = src.read(min(n, 1 << 20))
        if not chunk:
            sys.exit("truncated payload")
        n -= len(chunk)
        if dst:
            dst.write(chunk)


def target(out, name):
    """Path of an entry inside the output dir, or None for entries outside iPhoneOS.sdk."""
    i = name.find(SDK_DIR)
    if i < 0:
        return None
    rel = name[i + len(SDK_DIR):].split("/")
    if not rel[0].startswith("iPhoneOS") or not rel[0].endswith(".sdk"):
        return None
    # Other SDK dirs/links are aliases: everything is written to one iPhoneOS.sdk.
    if rel[0] != "iPhoneOS.sdk" and len(rel) == 1:
        return None
    if ".." in rel:
        sys.exit(f"unsafe path in archive: {name}")
    return os.path.join(out, "iPhoneOS.sdk", *rel[1:])


def extract(src, out):
    store = os.path.join(out, ".store")
    os.makedirs(store)
    pending = {}  # inode -> [(path, size, mode)] of filler entries in the SDK
    links = []
    count = 0
    while True:
        h = src.read(76)
        if len(h) < 76 or h[:6] != b"070707":
            sys.exit("bad cpio header")
        key = (h[6:12] + h[12:18]).decode()
        mode, nlink = int(h[18:24], 8), int(h[36:42], 8)
        name_len, size = int(h[59:65], 8), int(h[65:76], 8)
        name = src.read(name_len)[:-1].decode("utf-8", "surrogateescape")
        if name == "TRAILER!!!":
            break
        path = target(out, name)
        kind = mode & S_IFMT
        if kind == S_IFDIR:
            if path:
                os.makedirs(path, exist_ok=True)
        elif kind == S_IFLNK:
            data = src.read(size)
            if path:
                links.append((path, data.decode("utf-8", "surrogateescape")))
        elif kind != S_IFREG:
            copy(src, size, None)
        else:
            head = src.read(min(size, len(CANARY)))
            rest = size - len(head)
            if head.startswith(CANARY):
                copy(src, rest, None)
                if path:
                    pending.setdefault(key, []).append((path, size, mode))
                continue
            # Any SDK may hold the data for a filler entry of this one.
            stored = os.path.join(store, key)
            keep = nlink > 1 and ANY_SDK in name and not os.path.exists(stored)
            dst = stored if keep else path
            if dst:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(head)
                    copy(src, rest, f)
                count += 1
            else:
                copy(src, rest, None)
            if keep and path:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                shutil.copyfile(stored, path)
            if path:
                os.chmod(path, mode & 0o755 | 0o600)

    for key, entries in pending.items():
        stored = os.path.join(store, key)
        for path, size, mode in entries:
            if not os.path.exists(stored) or os.path.getsize(stored) != size:
                sys.exit(f"no data for hardlink {path}")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            shutil.copyfile(stored, path)
            os.chmod(path, mode & 0o755 | 0o600)
    for path, dest in links:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.lexists(path):
            os.symlink(dest, path)
    shutil.rmtree(store)
    return count, sum(len(v) for v in pending.values())


def fix_tbd(text):
    """Stubs list arm64e (and arm64e.x1, which LLVM rejects); the app is plain arm64."""
    def fix_list(m):
        seen = []
        for item in re.split(r"\s*,\s*", m.group(1).strip()):
            item = item.strip()
            if item.startswith("arm64e.") or not item:
                continue
            item = re.sub(r"^arm64e-", "arm64-", item)
            if item not in seen:
                seen.append(item)
        return "[ " + ", ".join(seen) + " ]"
    return re.sub(r"\[([^\[\]{}']*\barm64e[^\[\]{}']*)\]", fix_list, text)


def fix_tbds(root):
    n = 0
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if not f.endswith(".tbd") or os.path.islink(p):
                continue
            with open(p, encoding="utf-8", errors="surrogateescape") as fh:
                text = fh.read()
            fixed = fix_tbd(text)
            if fixed != text:
                with open(p, "w", encoding="utf-8", errors="surrogateescape") as fh:
                    fh.write(fixed)
                n += 1
    return n


def check(root):
    bad = 0
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if os.path.isfile(p) and not os.path.islink(p):
                with open(p, "rb") as fh:
                    if fh.read(len(CANARY)) == CANARY:
                        bad += 1
                        print("filler left:", p, file=sys.stderr)
    if bad:
        sys.exit(f"{bad} files still filler")


def main():
    out = sys.argv[1]
    _, linked = extract(sys.stdin.buffer, out)
    root = os.path.join(out, "iPhoneOS.sdk")
    tbds = fix_tbds(root)
    check(root)
    print(f"resolved {linked} hardlinks, adjusted {tbds} stubs")


main()
