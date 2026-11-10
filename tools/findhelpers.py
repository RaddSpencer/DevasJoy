"""Find the compiler's register save/restore helpers in a flat image (base 0x82000000)."""
import struct, sys
img = open(sys.argv[1], "rb").read()
BASE = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x82000000
pats = {
    "savegprlr_14": [0xF9C1FF68, 0xF9E1FF70],
    "restgprlr_14": [0xE9C1FF68, 0xE9E1FF70],
    "savefpr_14":   [0xD9CCFF70, 0xD9ECFF78],
    "restfpr_14":   [0xC9CCFF70, 0xC9ECFF78],
    "savevmx_14":   [0x3960FEE0, 0x7DCB61CE],
    "restvmx_14":   [0x3960FEE0, 0x7DCB60CE],
    "savevmx_64":   [0x3960FC00, 0x100B61CB],
    "restvmx_64":   [0x3960FC00, 0x100B60CB],
}
for name, words in pats.items():
    needle = b"".join(struct.pack(">I", w) for w in words)
    hits, i = [], img.find(needle)
    while i != -1:
        if i % 4 == 0: hits.append(BASE + i)
        i = img.find(needle, i + 4)
    print(f'{name}_address = {", ".join(f"0x{h:08X}" for h in hits) or "NOT FOUND"}')
