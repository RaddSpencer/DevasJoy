"""Write game\dlc\<package>.name (display name, UTF-8) from each DLC package header, for the host's content enumerator."""
import os, sys
src, dst = sys.argv[1], sys.argv[2]
for n in sorted(os.listdir(src)):
    h = open(os.path.join(src, n), "rb").read(0x500)
    name = h[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]
    open(os.path.join(dst, n + ".name"), "w", encoding="utf-8").write(name)
    print(n, name)
