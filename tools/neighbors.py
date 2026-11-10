import bisect, re, struct, sys
m = re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*sub_", open("ppc/ppc_func_mapping.cpp").read())
starts = sorted(int(x, 16) for x in m)
img = open("private/image.bin", "rb").read()
pd = img[0x821C5200 - 0x82000000:0x8220F2D8 - 0x82000000]
pdata = {}
for i in range(0, len(pd) - 7, 8):
    a, w = struct.unpack_from(">II", pd, i)
    if a: pdata[a] = ((w >> 8) & 0x3FFFFF) * 4
for t in sys.argv[1:]:
    t = int(t, 16); i = bisect.bisect_right(starts, t)
    print(f"--- around {t:08X}")
    for a in starts[max(0, i - 4):i + 3]:
        w = struct.unpack_from(">I", img, a - 0x82000000)[0]
        print(f"  {a:08X} pdata_len={pdata.get(a, '-')!s:>6} first={w:08X}")
