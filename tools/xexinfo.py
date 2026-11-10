"""Inspect an unencrypted, uncompressed XEX2 (e.g. XenonRecomp's default_patched.xex),
write its flat memory image, and report what a recompilation config needs.

usage: python xexinfo.py <patched.xex> <out_image.bin> [<imports.txt>]
"""
import struct
import sys

NAMES = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO", 0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT", 0x00010201: "IMAGE_BASE_ADDRESS", 0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP", 0x000183FF: "ORIGINAL_PE_NAME", 0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO", 0x00020200: "DEFAULT_STACK_SIZE", 0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE", 0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS", 0x00030000: "SYSTEM_FLAGS",
    0x00030100: "SYSTEM_FLAGS_32", 0x00030200: "SYSTEM_FLAGS_64", 0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE", 0x00040310: "GAME_RATINGS", 0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO", 0x000406FF: "MULTIDISC_MEDIA_IDS", 0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY", 0x00E10402: "EXPORTS_BY_NAME", 0x00008105: "DEVICE_ID",
    0x000080FF: "BOUNDING_PATH", 0x00040905: "?", 0x00010401: "?",
}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def main():
    x = open(sys.argv[1], "rb").read()
    assert x[:4] == b"XEX2"
    flags, pe_off, _, sec_off, nhdr = struct.unpack_from(">IIIII", x, 4)
    opt = {}
    for i in range(nhdr):
        k, v = struct.unpack_from(">II", x, 0x18 + 8 * i)
        opt[k] = v
    print(f"module flags {flags:08X}, PE data at 0x{pe_off:X}, {nhdr} optional headers")
    for k, v in opt.items():
        print(f"  {k:08X} {NAMES.get(k, '?'):30s} {v:08X}")

    image_size = u32(x, sec_off + 4)
    load_addr = u32(x, sec_off + 0x110)
    base = opt.get(0x00010201, load_addr)
    print(f"image base {base:08X}, size {image_size:08X} (ends {base + image_size:08X})")
    print(f"entry point {opt[0x00010100]:08X}")

    ffi = opt[0x000003FF]
    enc, comp = struct.unpack_from(">HH", x, ffi + 4)
    assert enc == 0 and comp == 1 or comp == 0, f"encryption {enc} compression {comp}: use the patched xex"
    data = x[pe_off:]
    if comp == 1:   # basic: (data, zero) block pairs
        info_size = u32(x, ffi)
        img = bytearray()
        p = 0
        for i in range((info_size - 8) // 8):
            dsz, zsz = struct.unpack_from(">II", x, ffi + 8 + 8 * i)
            img += data[p:p + dsz] + bytes(zsz)
            p += dsz
    else:
        img = bytearray(data[:image_size])
    img = bytes(img.ljust(image_size, b"\0"))
    open(sys.argv[2], "wb").write(img)
    print(f"wrote {sys.argv[2]} ({len(img)} bytes)")

    # PE sections
    pe = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, pe + 6)[0]
    opt_size = struct.unpack_from("<H", img, pe + 20)[0]
    s = pe + 24 + opt_size
    print("sections:")
    for i in range(nsec):
        name = img[s:s + 8].rstrip(b"\0").decode()
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", img, s + 8)
        ch = struct.unpack_from("<I", img, s + 36)[0]
        print(f"  {name:8s} {base + va:08X}-{base + va + vsize:08X} size {vsize:08X} flags {ch:08X}"
              f"{' CODE' if ch & 0x20 else ''}")
        s += 40

    if 0x00020104 in opt:
        o = opt[0x00020104]
        slots, raw, dsz, rsz = struct.unpack_from(">IIII", x, o)
        print(f"TLS: {slots} slots, raw data {raw:08X}, data size {dsz:X}, raw size {rsz:X}")
    for key in (0x00020200, 0x00020401, 0x00040201, 0x00040801):
        if key in opt:
            print(f"{NAMES[key]}: {opt[key]:X}")
    if 0x00040006 in opt:
        o = opt[0x00040006]
        media, ver, basever, title = struct.unpack_from(">IIII", x, o)
        print(f"execution info: media {media:08X} version {ver:08X} base {basever:08X} title {title:08X}")
    if 0x000183FF in opt:
        o = opt[0x000183FF]
        print("original PE name:", x[o + 4:o + u32(x, o)].split(b"\0")[0].decode())
    if 0x000200FF in opt:
        o = opt[0x000200FF]
        n = (u32(x, o) - 4) // 16
        print("static libraries:")
        for i in range(n):
            e = x[o + 4 + 16 * i:o + 20 + 16 * i]
            nm = e[:8].rstrip(b"\0").decode()
            a, b, c, d = struct.unpack_from(">HHHH", e, 8)
            print(f"  {nm:8s} {a}.{b}.{c}.{d & 0x7FFF}")

    # imports
    off = opt[0x000103FF]
    size, strsz, libcount = struct.unpack_from(">III", x, off)
    p = off + 12
    strtab = x[p:p + strsz]
    p += strsz
    names = [n.decode() for n in strtab.split(b"\0") if n]
    out = open(sys.argv[3], "w") if len(sys.argv) > 3 else None
    for li in range(libcount):
        lsize = u32(x, p)
        ver = u32(x, p + 0x1C)
        name_index, count = struct.unpack_from(">HH", x, p + 0x24)
        recs = struct.unpack_from(">%dI" % count, x, p + 0x28)
        nm = names[name_index] if name_index < len(names) else "?"
        funcs = sum(1 for a in recs if img[a - base] == 1)
        print(f"import library {li}: {nm} v{ver >> 28}.{ver >> 24 & 15}.{ver >> 8 & 0xFFFF}.{ver & 0xFF}"
              f" records {count} (functions {funcs}, variables {count - funcs})")
        if out:
            out.write(f"# library {li}: {nm} records={count}\n")
            for a in recs:
                w = u32(img, a - base)
                out.write(f"{li} {a:08X} word={w:08X} type={w >> 24} ordinal={w & 0xFFFF}\n")
        p += lsize


if __name__ == "__main__":
    main()
