"""Show the disassembly of the recompiled function that contains a guest address.

usage: python showfunc.py <hex address> [lines around the address, default 40] [--all]
Run from C:\\dev\\asura.
"""
import bisect, glob, re, sys

addr = int(sys.argv[1], 16)
around = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 40
whole = "--all" in sys.argv
starts = sorted(int(m, 16) for m in re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*sub_", open(r"ppc\ppc_func_mapping.cpp").read()))
fn = starts[bisect.bisect_right(starts, addr) - 1]
name = f"PPC_FUNC_IMPL(__imp__sub_{fn:08X})"
for p in glob.glob(r"ppc\ppc_recomp.*.cpp"):
    text = open(p, errors="ignore").read()
    i = text.find(name)
    if i < 0: continue
    j = text.find("PPC_FUNC_IMPL(", i + 10)
    body = text[i:j if j > 0 else len(text)].split("\n")
    lines = [l for l in body if l.strip().startswith("//") or l.startswith("loc_") or l.startswith("PPC_FUNC")]
    # disassembly comments carry no addresses: count instructions from the start
    idx = (addr - fn) // 4
    ins = [k for k, l in enumerate(lines) if l.strip().startswith("//")]
    print(f"{p}: sub_{fn:08X} (+0x{addr - fn:X}), {len(ins)} instructions")
    if whole:
        print("\n".join(lines)); break
    if idx < len(ins):
        k = ins[idx]
        for n, l in enumerate(lines[max(0, k - around):k + around // 2]):
            mark = ">>" if max(0, k - around) + n == k else "  "
            print(mark, l)
    break
