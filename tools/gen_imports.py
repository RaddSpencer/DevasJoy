r"""Generate host\generated\import_table.inc and import_stubs.cpp from private\imports.txt and ppc\ppc_func_mapping.cpp.
Import names come from the Xenia export tables kept in C:\dev\bcr2 (xboxkrnl_table.inc, xam_table.inc). Adapted from bcr2\gen_imports.py."""
import os, re
pat = re.compile(r"XE_EXPORT\(\s*\w+\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)")
names = {0: {}, 1: {}}
for lib, f in ((0, r"C:\dev\bcr2\xboxkrnl_table.inc"), (1, r"C:\dev\bcr2\xam_table.inc")):
    for m in pat.finditer(open(f, errors="ignore").read()):
        names[lib][int(m.group(1), 16)] = m.group(2)
recs = []
for l in open(r"private\imports.txt"):
    if l.startswith("#"): continue
    p = l.split()
    recs.append((int(p[0]), int(p[1], 16), int(p[3][5:]), int(p[4][8:])))
thunk = {(lib, o): a for lib, a, t, o in recs if t == 1}
libname = {0: "xboxkrnl", 1: "xam"}
nm = lambda lib, o: names[lib].get(o, f"{libname[lib]}_ord_{o}")
with open(r"host\generated\import_table.inc", "w") as f:
    for lib, a, t, o in recs:
        if t == 0:
            f.write(f'    {{ 0x{a:08X}, 0x{thunk.get((lib, o), 0):08X}, {lib}, {o}, "{nm(lib, o)}" }},\n')
mapping = {int(m.group(1), 16): m.group(2) for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+),\s*(\w+)\s*\}", open(r"ppc\ppc_func_mapping.cpp").read())}
seen = set()
with open(r"host\generated\import_stubs.cpp", "w") as f:
    f.write('#include "ppc_context.h"\n#include <cstdint>\nvoid ppc_import_dispatch(PPCContext& ctx, uint8_t* base, int lib, int ordinal);\n\n')
    for (lib, o), a in sorted(thunk.items(), key=lambda kv: kv[1]):
        sym = mapping.get(a, f"sub_{a:08X}")
        if sym in seen: continue
        seen.add(sym)
        f.write(f"PPC_FUNC({sym}) {{ ppc_import_dispatch(ctx, base, {lib}, {o}); }}\n")
print(len(thunk), "function thunks,", sum(1 for r in recs if r[2] == 0 and (r[0], r[3]) not in thunk), "variables")
