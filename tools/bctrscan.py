"""Classify the instruction sequences that precede each bctr in the code sections."""
import collections
import struct
import sys

img = open(sys.argv[1], "rb").read()
B = 0x82000000
LO, HI = 0x82220000, 0x82E514AC
words = struct.unpack_from(">%dI" % ((HI - LO) // 4), img, LO - B)


def op(w):
    p = w >> 26
    if p == 15: return "addis" if (w >> 16) & 31 else "lis"
    if p == 14: return "addi" if (w >> 16) & 31 else "li"
    if p == 21: return "rlwinm"
    if p == 11: return "cmpwi"
    if p == 10: return "cmplwi"
    if p == 16: return "bc"
    if p == 18: return "b"
    if p == 32: return "lwz"
    if p == 34: return "lbz"
    if p == 40: return "lhz"
    if p == 31:
        x = (w >> 1) & 1023
        return {23: "lwzx", 87: "lbzx", 279: "lhzx", 343: "lhax", 266: "add", 467: "mtspr", 444: "or",
                24: "slw", 40: "subf", 0: "cmpw", 32: "cmplw", 339: "mfspr", 824: "srawi",
                922: "extsh", 954: "extsb"}.get(x, f"x{x}")
    return f"p{p}"


cnt = collections.Counter()
ex = {}
n = 0
for i, w in enumerate(words):
    if w == 0x4E800420 and i >= 7:
        n += 1
        seq = tuple(op(words[i - k]) for k in range(7, 0, -1))
        cnt[seq] += 1
        ex.setdefault(seq, LO + 4 * i)
print("bctr total", n)
for seq, c in cnt.most_common(20):
    print(c, hex(ex[seq]), " ".join(seq))
