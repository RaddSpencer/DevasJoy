"""Xbox 360 STFS (LIVE/PIRS/CON) package lister and extractor.
Ported from the BCR2 host extractor (C:\\dev\\bcr2\\host\\extract.cpp).

usage:
  python stfs.py <package> info
  python stfs.py <package> list
  python stfs.py <package> extract <outdir>
"""
import os
import struct
import sys


def be24(b, o):
    return b[o] << 16 | b[o + 1] << 8 | b[o + 2]


def le24(b, o):
    return b[o] | b[o + 1] << 8 | b[o + 2] << 16


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x1000)
        if hdr[:4] not in (b"LIVE", b"PIRS", b"CON "):
            raise SystemExit("not an STFS package")
        self.hdr = hdr
        self.title_id = struct.unpack_from(">I", hdr, 0x360)[0]
        self.content_type = struct.unpack_from(">I", hdr, 0x344)[0]
        self.display_name = hdr[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]
        header_size = struct.unpack_from(">I", hdr, 0x340)[0]
        self.base = (header_size + 0xFFF) & 0xF000
        vd = hdr[0x379:]
        if vd[0] != 0x24:
            raise SystemExit("unexpected volume descriptor (SVOD package?)")
        self.shift = 0 if vd[2] & 1 else 1
        ft_blocks = vd[3] | vd[4] << 8
        ft_start = le24(vd, 5)
        ft = b"".join(self.block(ft_start + i) for i in range(ft_blocks))
        self.entries = []
        for o in range(0, len(ft), 0x40):
            e = ft[o:o + 0x40]
            nl = e[0x28] & 0x3F
            if not nl:
                break
            self.entries.append(dict(
                name=e[:nl].decode("latin-1"),
                consecutive=bool(e[0x28] & 0x40), dir=bool(e[0x28] & 0x80),
                blocks=le24(e, 0x29), start=le24(e, 0x2F),
                parent=struct.unpack_from(">H", e, 0x32)[0],
                size=struct.unpack_from(">I", e, 0x34)[0]))

    def backing(self, n):
        b = n + (((n + 0xAA) // 0xAA) << self.shift)
        if n >= 0xAA:
            b += ((n + 0x70E4) // 0x70E4) << self.shift
        if n >= 0x70E4:
            b += ((n + 0x4AF768) // 0x4AF768) << self.shift
        return b

    def offset(self, n):
        return self.base + self.backing(n) * 0x1000

    def block(self, n):
        self.f.seek(self.offset(n))
        return self.f.read(0x1000)

    def next_block(self, n):
        table = self.offset(n // 0xAA * 0xAA) - 0x1000
        self.f.seek(table + (n % 0xAA) * 24)
        return be24(self.f.read(24), 0x15)

    def path_of(self, i):
        e = self.entries[i]
        p = e["name"]
        par = e["parent"]
        guard = 0
        while par != 0xFFFF and par < len(self.entries) and guard < 64:
            p = self.entries[par]["name"] + "/" + p
            par = self.entries[par]["parent"]
            guard += 1
        return p

    def read_file(self, e, out):
        remaining = e["size"]
        blk = e["start"]
        while remaining:
            n = min(remaining, 0x1000)
            out.write(self.block(blk)[:n])
            remaining -= n
            if remaining:
                blk = blk + 1 if e["consecutive"] else self.next_block(blk)


def main():
    s = Stfs(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "info":
        print(f"title {s.title_id:08X} type {s.content_type:08X} name {s.display_name!r} files {len(s.entries)}")
        return
    for i, e in enumerate(s.entries):
        p = s.path_of(i)
        if cmd == "list":
            print(f"{'D' if e['dir'] else 'F'} {e['size']:12d} {p}")
            continue
        dst = os.path.join(sys.argv[3], *p.split("/"))
        if e["dir"]:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as o:
            s.read_file(e, o)
        print(f"{e['size']:12d} {p}")


if __name__ == "__main__":
    main()
