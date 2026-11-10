r"""Name Asura's imports (Xenia tables in C:\dev\bcr2) and compare with what the BCR2 host implements."""
import re
def table(p):
    return {int(o, 16): n for o, n in re.findall(r"XE_EXPORT\(\s*\w+\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)", open(p).read())}
names = {0: table(r"C:\dev\bcr2\xboxkrnl_table.inc"), 1: table(r"C:\dev\bcr2\xam_table.inc")}
impl = set(re.findall(r'register_by_name\("(\w+)"', open(r"C:\dev\bcr2\host\imports.cpp").read()))
bcr2 = set(re.findall(r'"(\w+)" \}', open(r"C:\dev\bcr2\host\generated\import_table.inc").read()))
funcs, vars_ = {0: set(), 1: set()}, {0: set(), 1: set()}
for l in open("private/imports.txt"):
    if l.startswith("#"): continue
    lib, a, w, t, o = l.split()
    (funcs if t == "type=1" else vars_)[int(lib)].add(int(o.split("=")[1]))
out = open("private/imports_named.txt", "w")
missing = []
for lib in (0, 1):
    for o in sorted(funcs[lib] | vars_[lib]):
        n = names[lib].get(o, f"ord{o}")
        kind = "fn " if o in funcs[lib] else "var"
        st = "impl" if n in impl else ("bcr2-stub" if n in bcr2 else "NEW")
        out.write(f"{['xboxkrnl','xam'][lib]} {o:4d} {kind} {st:9s} {n}\n")
        if o in funcs[lib] and st != "impl": missing.append((lib, n, st))
print("functions:", sum(len(funcs[l]) for l in (0, 1)), "variables:", sum(len(vars_[l] - funcs[l]) for l in (0, 1)))
print("implemented in BCR2 host:", sum(len(funcs[l]) for l in (0, 1)) - len(missing))
print("imported by BCR2 too but not implemented (stubbed):", sum(1 for m in missing if m[2] == "bcr2-stub"))
print("new to Asura:", sum(1 for m in missing if m[2] == "NEW"))
for lib, n, st in missing:
    if st == "NEW": print("  ", ["xboxkrnl", "xam"][lib], n)
