// Overrides of recompiled game functions (XenonRecomp emits them as weak symbols; __imp__ is the original).
#include "kernel.h"
#include <windows.h>

static std::string guest_wide(uint32_t p, size_t max = 2048) {
    std::string s;
    for (size_t i = 0; p && i < max; i++) { uint16_t c = rd16(p + (uint32_t)i * 2); if (!c) break; s += c < 0x80 ? (char)c : '?'; }
    return s;
}
static std::string guest_text(uint32_t p) {   // ASCII or UTF-16, whichever the bytes look like
    if (!p || IsBadReadPtr(g_base + p, 4)) return "(null)";
    return (g_base[p] == 0 && g_base[p + 1] != 0) ? guest_wide(p) : guest_cstr(p, 2048);
}

// UE3 fatal error (appError path): prints the message and parks the thread with Sleep(INFINITE). Log it, always.
PPC_FUNC_IMPL(__imp__sub_8229BB28);
PPC_FUNC(sub_8229BB28) {
    fprintf(stderr, "[fatal] %s | %s (lr=%08X)\n", guest_text(ctx.r4.u32).c_str(), guest_text(ctx.r5.u32).c_str(), (uint32_t)ctx.lr);
    fflush(stderr);
    __imp__sub_8229BB28(ctx, base);
}
