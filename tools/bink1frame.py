"""Make one-frame copies of Bink 1 movies (header and first frame only), for the host's ASURA_SKIPINTRO.

The game treats a missing or empty start-up movie as a fatal I/O error, so the host serves these instead.
Bink 1 layout: 'BIK?', u32 size-8, u32 frames, u32 largest frame, u32 frames, width, height, fps num, fps den, flags,
u32 audio tracks; per track three u32 (max decoded size, sample rate+flags, track id); then (frames+1) u32 frame offsets
(bit 0 = keyframe), then the frames.
usage: python bink1frame.py <outdir> <movie.bik> [...]
"""
import os, struct, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for path in sys.argv[2:]:
    d = open(path, "rb").read()
    assert d[:3] == b"BIK", path
    frames = struct.unpack_from("<I", d, 8)[0]
    tracks = struct.unpack_from("<I", d, 40)[0]
    table = 44 + tracks * 12
    off0, off1 = struct.unpack_from("<II", d, table)
    frame = d[off0 & ~1:off1 & ~1]
    hdr = bytearray(d[:table])
    newTable = table + 8
    struct.pack_into("<I", hdr, 8, 1)
    struct.pack_into("<I", hdr, 12, len(frame))
    struct.pack_into("<I", hdr, 16, 1)
    body = bytes(hdr) + struct.pack("<II", newTable | (off0 & 1), newTable + len(frame)) + frame
    body = body[:4] + struct.pack("<I", len(body) - 8) + body[8:]
    dst = os.path.join(out, os.path.basename(path))
    open(dst, "wb").write(body)
    print(f"{os.path.basename(path)}: {frames} frames, {tracks} audio tracks -> 1 frame, {len(body)} bytes")
