r"""Copy the BCR2 D3D11 Xenos renderer into the Asura host, adapted.

The renderer stays as close to BCR2's as possible so the two can later become one shared file. What changes:
  - debug switches BCR2_* become ASURA_*
  - guest memory is reached through gpu_phys_to_host() (the 0xE0000000 window's 0x1000 offset, see NOTES.md)
  - the window title and class
  - BCR2-only hooks (settings menu, keyboard/mouse, button prompts, data paths) come from xenos_glue.cpp
usage: python port_renderer.py   (from C:\dev\asura)
"""
import re, shutil

SRC = r"C:\dev\bcr2\host"
DST = r"C:\dev\asura\host"

s = open(SRC + r"\gpu.cpp", encoding="utf-8").read()
s = s.replace('getenv("BCR2_', 'getenv("ASURA_').replace('L"BCR2_', 'L"ASURA_')
old_phys = "static inline const uint8_t* phys(uint32_t a) { return g_base + (0xA0000000u | (a & 0x1FFFFFFFu)); }"
assert old_phys in s
s = s.replace(old_phys,
    "uint32_t gpu_phys_to_host(uint32_t p);   // kernel.cpp: physical address -> guest address (E0 window pages are 0x1000 behind)\n"
    "static inline const uint8_t* phys(uint32_t a) { return g_base + gpu_phys_to_host(a); }")
s = s.replace('L"bcr2win"', 'L"asurawin"').replace('L"Bionic Commando Rearmed 2 (recomp)"', 'L"Asura\'s Wrath (recomp)"')
header = ("// Xenos (Xbox 360 GPU) renderer on Direct3D 11. Ported from the BCR2 host (C:\\dev\\bcr2\\host\\gpu.cpp) by tools\\port_renderer.py:\n"
          "// keep it close to that file so the two can merge into one shared renderer. Asura-specific glue is in xenos_glue.cpp.\n")
s = header + s
open(DST + r"\xenos.cpp", "w", encoding="utf-8").write(s)

for f in ("gpu.h", "shaderpack.h", "shaderpack.cpp", "kbm.h", "paths.h"):
    shutil.copyfile(SRC + "\\" + f, DST + "\\" + f)
print("xenos.cpp:", s.count("\n"), "lines,", s.count('getenv("ASURA_'), "switches")
