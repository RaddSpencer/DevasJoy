#!/bin/sh
# Run after regenerating ppc\ (XenonRecomp output): bash tools/patch_e0_alias.sh
#
# On the console the 0xE0000000 window (4K pages) is 0x1000 ahead of physical memory: virtual E0000000+x is physical x+0x1000.
# The host maps all three windows (A0, C0, E0) at the same section offset (views have to be 64K aligned), so here E0000000+x is
# the same memory as C0000000+x. D3D code that turns an E0 address into a C0 (cached, CPU) pointer adds the console's 0x1000:
#     rlwinm rA,rB,12,20,31 / clrlwi rB,rB,3 / addi rA,rA,512 / rlwinm rA,rA,0,19,19 / add rB,rA,rB / addis rB,rB,-16384
# and reads one page past the data. Query results were read that way (GetData, sub_8299FAF8): always zero samples, so the game
# culled every object it occlusion-tested. This makes the +512 a +0 wherever the result becomes a CPU pointer (an addis -16384
# within 10 lines). The same idiom feeding a GPU (physical) address is left alone: the GPU side expects console addresses.
set -e
cd "$(dirname "$0")/../ppc"
n=0
for f in ppc_recomp.*.cpp; do
    awk '
        { line[NR] = $0 }
        /\/\/ addi r[0-9]+,r[0-9]+,512$/ { site = NR }
        /\/\/ addis r[0-9]+,r[0-9]+,-16384$/ && site && NR - site <= 10 { fix[site + 1] = 1; site = 0 }
        END {
            for (i = 1; i <= NR; i++) {
                if (fix[i] && line[i] ~ /\+ 512;$/) { sub(/\+ 512;$/, "+ 0;   // E0 alias: see tools/patch_e0_alias.sh", line[i]); patched++ }
                print line[i]
            }
            if (patched) printf "%d\n", patched > "/dev/stderr"
        }' "$f" > "$f.tmp" 2> "$f.count"
    c=$(cat "$f.count"); rm -f "$f.count"
    if [ -n "$c" ]; then mv "$f.tmp" "$f"; n=$((n + c)); echo "$f: $c"; else rm -f "$f.tmp"; fi
done
echo "patched $n E0-to-C0 conversions"

# Also: let the profiling build (host/funcprof.h, -DASURA_FUNCPROF=ON) supply its own PPC_FUNC_PROLOGUE.
if ! grep -q "ifndef PPC_FUNC_PROLOGUE" ppc_context.h; then
    sed -i 's|^#define PPC_FUNC_PROLOGUE() __builtin_assume(((size_t)base & 0x1F) == 0)$|#ifndef PPC_FUNC_PROLOGUE   // the profiling build defines its own (host/funcprof.h)\n&\n#endif|' ppc_context.h
    echo "guarded PPC_FUNC_PROLOGUE in ppc_context.h"
fi
