"""Convert renderer BMP captures to PNG, optionally as one contact sheet, or keep their alpha channel.

usage: python bmp2png.py <out.png> <scale divisor> <bmp> [<bmp> ...]
       python bmp2png.py --alpha <out.png> <bmp>          (one 32-bit BMP to a transparent PNG, full size)
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

def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
def write_png(out, W, H, lines, channels):
    raw = zlib.compress(b"".join(b"\0" + bytes(l) for l in lines), 6)
    ctype = 6 if channels == 4 else 2
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, ctype, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b""))

if sys.argv[1] == "--alpha":
    out, path = sys.argv[2], sys.argv[3]
    w, h, bpp, rows = load(path)
    assert bpp == 4, "needs a 32-bit BMP"
    lines = []
    for r in rows:
        l = bytearray(w * 4)
        for x in range(w):
            b, g, rr, a = r[x * 4:x * 4 + 4]
            l[x * 4:x * 4 + 4] = bytes((rr, g, b, a))
        lines.append(l)
    write_png(out, w, h, lines, 4)
    print(out, w, h, "RGBA")
    sys.exit(0)

out, div, paths = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
imgs = [load(p) for p in paths]
w0, h0 = imgs[0][0] // div, imgs[0][1] // div
cols = min(3, len(imgs)); nrows = (len(imgs) + cols - 1) // cols
W, H = w0 * cols, h0 * nrows
canvas = [bytearray(W * 3) for _ in range(H)]
for i, (w, h, bpp, rows) in enumerate(imgs):
    ox, oy = (i % cols) * w0, (i // cols) * h0
    for y in range(min(h0, h // div)):
        r = rows[y * div]
        line = canvas[oy + y]
        for x in range(min(w0, w // div)):
            p = x * div * bpp
            line[(ox + x) * 3:(ox + x) * 3 + 3] = bytes((r[p + 2], r[p + 1], r[p]))
write_png(out, W, H, canvas, 3)
print(out, W, H)
