"""For each '// ERROR <target>' in the output, show the source function, the function the target falls in, and the gap."""
import bisect, glob, re
starts = sorted(int(m, 16) for m in re.findall(r"\{\s*0x([0-9A-Fa-f]+),", open("ppc/ppc_func_mapping.cpp").read()))
seen = set()
for p in sorted(glob.glob("ppc/ppc_recomp.*.cpp")):
    fn = None
    for l in open(p, errors="ignore"):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", l)
        if m: fn = int(m.group(1), 16)
        m = re.search(r"// ERROR ([0-9A-F]+)", l)
        if m and (fn, m.group(1)) not in seen:
            seen.add((fn, m.group(1)))
            t = int(m.group(1), 16)
            i = bisect.bisect_right(starts, fn)
            nxt = starts[i] if i < len(starts) else 0
            j = bisect.bisect_right(starts, t) - 1
            print(f"sub_{fn:08X} (next fn {nxt:08X}) -> {t:08X} in sub_{starts[j]:08X} +0x{t - starts[j]:X}")
