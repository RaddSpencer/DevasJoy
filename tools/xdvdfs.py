"""Xbox 360 disc image (XDVDFS / XGD) lister and extractor.

usage:
  python xdvdfs.py <iso> list
  python xdvdfs.py <iso> extract <outdir> [path-prefix ...]
"""
import os
import struct
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 0x800
# game partition offsets: plain image, XGD3, XGD2, XGD1
PARTITIONS = [0, 0x2080000, 0xFD90000, 0x18300000]


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("no XDVDFS volume descriptor found")


def walk(f, base, sector, size, prefix, out):
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF:
            continue
        name = data[off + 14:off + 14 + nlen].decode("latin-1")
        path = prefix + name
        if attr & 0x10:
            out.append((path + "/", sec, sz, True))
            walk(f, base, sec, sz, path + "/", out)
        else:
            out.append((path, sec, sz, False))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def entries(f):
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + 20)
    root_sec, root_size = struct.unpack("<II", f.read(8))
    out = []
    walk(f, base, root_sec, root_size, "", out)
    out.sort()
    return base, out


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base, ents = entries(f)
        if cmd == "list":
            print(f"partition offset 0x{base:X}")
            for path, sec, sz, is_dir in ents:
                print(f"{'D' if is_dir else 'F'} {sz:12d} {path}")
            return
        outdir = sys.argv[3]
        prefixes = sys.argv[4:]
        for path, sec, sz, is_dir in ents:
            if prefixes and not any(path.lower().startswith(p.lower()) for p in prefixes):
                continue
            dst = os.path.join(outdir, *path.rstrip("/").split("/"))
            if is_dir:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(base + sec * SECTOR)
            left = sz
            with open(dst, "wb") as o:
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    if not chunk:
                        raise SystemExit(f"short read in {path}")
                    o.write(chunk)
                    left -= len(chunk)
            print(f"{sz:12d} {path}")


if __name__ == "__main__":
    main()
