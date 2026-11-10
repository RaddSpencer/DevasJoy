import struct, collections
img = open("private/image.bin", "rb").read(); B = 0x82000000
pd = img[0x821C5200 - B:0x8220F2D8 - B]
c = collections.Counter(); n = 0
for i in range(0, len(pd) - 7, 8):
    a, w = struct.unpack_from(">II", pd, i)
    if not a: continue
    n += 1
    if (w >> 31) & 1:  # ExceptionFlag
        h, s = struct.unpack_from(">II", img, a - 8 - B)
        c[h] += 1
print(n, "pdata entries;", sum(c.values()), "with ExceptionFlag; handler words before them:")
for h, k in c.most_common(): print(f"  {h:08X} x{k}")
