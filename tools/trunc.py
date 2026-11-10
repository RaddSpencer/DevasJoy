"""Find recompiled functions whose last instruction is a conditional return (beqlr, bdzlr, ...): XenonRecomp may have ended them
early and split the rest off (the BCR2 memset bug). Prints each with the function that follows it.
Run from C:\\dev\\asura."""
import glob, re
funcs = []
for p in glob.glob(r"ppc\ppc_recomp.*.cpp"):
    cur, last = None, None
    for l in open(p, errors="ignore"):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", l)
        if m:
            if cur is not None: funcs.append((cur, last))
            cur, last = int(m.group(1), 16), None
            continue
        s = l.strip()
        if s.startswith("// ") and cur is not None: last = s[3:]
    if cur is not None: funcs.append((cur, last))
funcs.sort()
uncond = {"blr", "b", "bctr"}
for i, (a, last) in enumerate(funcs):
    if not last: continue
    op = last.split()[0].rstrip("+-")
    if op.endswith("lr") and op not in ("blr", "mflr", "mtlr") and not op.endswith("lrl"):
        nxt = funcs[i + 1][0] if i + 1 < len(funcs) else 0
        print(f"sub_{a:08X} ends with '{last}', next function sub_{nxt:08X}")
