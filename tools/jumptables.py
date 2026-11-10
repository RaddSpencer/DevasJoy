"""Find jump tables at every bctr in the code sections and write a XenonRecomp switch table TOML.

Handles absolute tables (ctr = word loaded from table[idx*4]) and offset tables
(ctr = code_base + (table[idx] << shift), byte or halfword entries), in any
instruction order, by tracking register contents over the 40 instructions before the bctr.
Generalised from C:\\dev\\bcr2\\bctr_u16.py.

usage: python jumptables.py <image.bin> <out.toml> <unresolved.txt>
"""
import struct
import sys

BASE = 0x82000000
CODE = [(0x82220000, 0x82E3D2D4), (0x82E3D400, 0x82E50868), (0x82E50A00, 0x82E514AC)]
d = open(sys.argv[1], "rb").read()


def W(a): return struct.unpack_from(">I", d, a - BASE)[0]
def H(a): return struct.unpack_from(">H", d, a - BASE)[0]
def Bt(a): return d[a - BASE]
def sx16(v): return v - 0x10000 if v & 0x8000 else v
def in_code(a): return any(lo <= a < hi for lo, hi in CODE)


def analyze(bc):
    st, alias = {}, {}

    def idx_of(r):
        s = st.get(r)
        if s and s[0] == "idx": return s[1], s[2]
        if s and s[0] in ("ld", "tgt", "c"): return None
        return alias.get(r, r), 1

    for a in range(bc - 4 * 40, bc, 4):
        w = W(a); op = w >> 26
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31
        xo10 = (w >> 1) & 0x3FF
        if (op == 18 and not w & 1) or w in (0x4E800020, 0x4E800420):
            # unconditional b / blr / bctr: what follows is only reached through a branch target
            st, alias = {}, {}
            continue
        if (op == 18 and w & 1) or w in (0x4E800021, 0x4E800421):
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):  # a call clobbers the volatile registers
                st.pop(r, None); alias.pop(r, None)
            continue
        if op == 16:
            continue  # conditional branch: keep state (tables follow their bounds check)
        if op == 15:
            if rA == 0: st[rD] = ("c", (sx16(w & 0xFFFF) << 16) & 0xFFFFFFFF)
            elif st.get(rA, ("",))[0] == "c": st[rD] = ("c", (st[rA][1] + (sx16(w & 0xFFFF) << 16)) & 0xFFFFFFFF)
            else: st.pop(rD, None)
        elif op == 14:
            if rA == 0: st[rD] = ("c", sx16(w & 0xFFFF) & 0xFFFFFFFF)
            elif st.get(rA, ("",))[0] == "c": st[rD] = ("c", (st[rA][1] + sx16(w & 0xFFFF)) & 0xFFFFFFFF)
            else: st.pop(rD, None)
        elif op == 24:  # ori rA,rS,uimm
            if rD == rA and (w & 0xFFFF) == 0: pass
            elif st.get(rD, ("",))[0] == "c": st[rA] = ("c", st[rD][1] | (w & 0xFFFF))
            else: st.pop(rA, None)
        elif op == 21:  # rlwinm rA,rS,SH,MB,ME
            sh = (w >> 11) & 31; src = st.get(rD)
            if src and src[0] == "ld": st[rA] = ("ld", src[1], src[2], src[3], src[4] + sh, src[5])
            elif src and src[0] == "idx": st[rA] = ("idx", src[1], src[2] << sh)
            elif src and src[0] in ("c", "tgt"): st.pop(rA, None)
            elif sh == 0:  # a mask (e.g. clrlwi): the masked register is itself the switch index
                st.pop(rA, None); alias.pop(rA, None)
            else: st[rA] = ("idx", alias.get(rD, rD), 1 << sh)
        elif op == 31 and xo10 in (23, 279, 87):  # lwzx / lhzx / lbzx
            size = {23: 4, 279: 2, 87: 1}[xo10]
            res = None
            for cr_, ir in ((rA, rB), (rB, rA)):
                if res is None and st.get(cr_, ("",))[0] == "c":
                    i = idx_of(ir)
                    if i: res = ("ld", st[cr_][1], i[0], i[1], 0, size)
            if res: st[rD] = res
            else: st.pop(rD, None)
        elif op == 31 and ((w >> 1) & 0x1FF) == 266:  # add
            x, y = st.get(rA), st.get(rB)
            if x and y and x[0] == "ld" and y[0] == "c": st[rD] = ("tgt", x, y[1])
            elif x and y and y[0] == "ld" and x[0] == "c": st[rD] = ("tgt", y, x[1])
            else: st.pop(rD, None)
        elif op == 31 and xo10 == 467:  # mtspr
            spr = (((w >> 11) & 31) << 5) | ((w >> 16) & 31)
            if spr == 9: st["ctr"] = st.get(rD)
        elif op == 31 and xo10 == 444 and rD == rB:  # mr
            alias[rA] = alias.get(rD, rD)
            if rD in st: st[rA] = st[rD]
            else: st.pop(rA, None)
        elif op in (32, 33, 34, 35, 40, 41, 42, 43, 58) or (op == 31 and xo10 not in (0, 32)):
            st.pop(rD, None)
            if op == 31: st.pop(rA, None)

    t = st.get("ctr")
    if not t: return None, "ctr not from a table"
    if t[0] == "ld" and t[5] == 4:
        _, tbl, root, mult, shift, size = t
        code, kind = None, "absolute"
        if mult != 4 or shift: return None, f"absolute table with index multiplier {mult}"
    elif t[0] == "tgt":
        _, tbl, root, mult, shift, size = t[1]
        code, kind = t[2], "offset"
        if mult != size: return None, f"index multiplier {mult} vs element size {size}"
    else:
        return None, "ctr not from a table"

    if not any(lo <= tbl < hi for lo, hi in [(0x82000600, 0x821C50DC)] + CODE):
        return None, f"table {tbl:08X} is in writable data (filled at run time)"
    cands = {root} | {r for r, v in alias.items() if v == root}
    n = None; breg = root
    for a in range(bc - 4, bc - 4 * 48, -4):
        w = W(a)
        if (w >> 26) == 10 and ((w >> 16) & 31) in cands and not (w >> 21) & 1:
            n = (w & 0xFFFF) + 1; breg = (w >> 16) & 31; break
    if n is None and kind == "absolute" and tbl == bc + 4:
        # inline table with no bound check in reach: it runs until the first word that is not a nearby label
        n = 0
        while in_code(W(tbl + 4 * n)) and W(tbl + 4 * n) % 4 == 0 and abs(W(tbl + 4 * n) - bc) < 0x10000:
            n += 1
        if not n: return None, "inline table is empty"
        print(f"note: bctr {bc:08X} inline table without bound, {n} entries (r{root})")
    if n is None: return None, "no cmplwi bound"
    labels = []
    for i in range(n):
        if kind == "absolute": lab = W(tbl + 4 * i)
        else:
            v = H(tbl + 2 * i) if size == 2 else Bt(tbl + i)
            lab = (code + (v << shift)) & 0xFFFFFFFF
        if lab % 4 or not in_code(lab) or abs(lab - bc) > 0x80000:
            return None, f"bad label {lab:08X} at entry {i}"
        labels.append(lab)
    return dict(base=bc, r=breg, labels=labels, tbl=tbl, n=n, kind=kind, size=size), None


ok, un = [], []
for lo, hi in CODE:
    words = struct.unpack_from(">%dI" % ((hi - lo) // 4), d, lo - BASE)
    for i, w in enumerate(words):
        if w != 0x4E800420: continue
        a = lo + 4 * i
        r, why = analyze(a)
        (ok.append(r) if r else un.append((a, why)))

with open(sys.argv[2], "w") as out:
    out.write("# generated by tools/jumptables.py from the TU-patched image\n\n")
    for r in ok:
        out.write("[[switch]]\nbase = 0x%X\nr = %d\nlabels = [\n%s]\n\n"
                  % (r["base"], r["r"], "".join("    0x%X,\n" % l for l in r["labels"])))
kinds = {}
for r in ok: kinds[(r["kind"], r["size"])] = kinds.get((r["kind"], r["size"]), 0) + 1
reasons = {}
for a, why in un: reasons[why.split(" ")[0] + " " + why.split(" ")[1]] = reasons.get(why.split(" ")[0] + " " + why.split(" ")[1], 0) + 1
print(len(ok), "tables", kinds)
print(len(un), "other bctr:", reasons)
with open(sys.argv[3], "w") as f:
    for a, why in un:
        f.write("%08X %s | %s\n" % (a, why, " ".join("%08X" % W(a - 4 * k) for k in range(12, 0, -1))))
