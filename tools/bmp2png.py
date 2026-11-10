"""Convert renderer BMP captures to PNG, optionally as one contact sheet.

usage: python bmp2png.py <out.png> <scale divisor> <bmp> [<bmp> ...]
Several inputs are tiled 3 across, each shrunk by the divisor (2 -> 640x360 from 1280x720)."""
import struct, sys, zlib

def load(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = (abs(h) - 1 - y) if h > 0 else y
        rows.append(d[off + sy * stride: off + sy * stride + w * bpp])
    return w, abs(h), bpp, rows

out, div, paths = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
imgs = [load(p) for p in paths]
w0, h0 = imgs[0][0] // div, imgs[0][1] // div
cols = min(3, len(imgs)); nrows = (len(imgs) + cols - 1) // cols
W, H = w0 * cols, h0 * nrows
canvas = [bytearray(W * 3) for _ in range(H)]
for i, (w, h, bpp, rows) in enumerate(imgs):
    ox, oy = (i % cols) * w0, (i // cols) * h0
    for y in range(h0):
        r = rows[y * div]
        line = canvas[oy + y]
        for x in range(w0):
            p = x * div * bpp
            line[(ox + x) * 3:(ox + x) * 3 + 3] = bytes((r[p + 2], r[p + 1], r[p]))
raw = zlib.compress(b"".join(b"\0" + bytes(l) for l in canvas), 6)
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b""))
print(out, W, H)
