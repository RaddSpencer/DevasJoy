// Overrides of recompiled game functions (XenonRecomp emits them as weak symbols; __imp__ is the original).
#include "kernel.h"
#include <windows.h>
#include <cmath>
#include <atomic>
#include <map>
#include <string>
#include <mutex>

static std::string guest_wide(uint32_t p, size_t max = 2048) {
    std::string s;
    for (size_t i = 0; p && i < max; i++) { uint16_t c = rd16(p + (uint32_t)i * 2); if (!c) break; s += c < 0x80 ? (char)c : '?'; }
    return s;
}
static std::string guest_text(uint32_t p) {   // ASCII or UTF-16, whichever the bytes look like
    if (!p || IsBadReadPtr(g_base + p, 4)) return "(null)";
    return (g_base[p] == 0 && g_base[p + 1] != 0) ? guest_wide(p) : guest_cstr(p, 2048);
}

// Write watch for one guest word (ASURA_CULLWATCH=<entry index>, armed by the cull dump below at its first frame): its page is
// made read-only; each write to the word logs the host code address (look it up in asura.map), the guest lr and the values.
extern thread_local PPCContext* g_ctx;
// Watches a whole table of 156-byte entries [g_watchLo, g_watchHi): logs every write that turns an entry's word at
uint64_t gpu_frame_number();
// g_watchField from nonzero to zero (the code address, the guest lr, the entry index). Other writes are stepped silently.
static uint32_t g_watchLo = 0, g_watchHi = 0, g_watchField = 56;
static thread_local bool t_watchStep = false;
static thread_local uint32_t t_watchWord = 0, t_watchOld = 0;
static thread_local uintptr_t t_watchRip = 0;
static void watch_protect(DWORD prot) {
    DWORD old; const uint32_t lo = g_watchLo & ~0xFFFu, hi = (g_watchHi + 0xFFFu) & ~0xFFFu;
    VirtualProtect(g_base + lo, hi - lo, prot, &old);
}
static LONG CALLBACK watch_handler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!g_watchHi) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t lo = g_watchLo & ~0xFFFu, hi = (g_watchHi + 0xFFFu) & ~0xFFFu;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] == 1 && !t_watchStep) {
        uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (a >= g_base + lo && a < g_base + hi) {
            const uint32_t ga = (uint32_t)(a - g_base);
            t_watchWord = 0;
            if (ga >= g_watchLo && ga < g_watchHi) {
                const uint32_t w = ga - ((ga - g_watchLo) % 156) + g_watchField;   // the watched word of this entry
                if (ga + 4 > w && ga < w + 4) { t_watchWord = w; t_watchOld = rd32(w); t_watchRip = ep->ContextRecord->Rip; }
            }
            watch_protect(PAGE_READWRITE);
            ep->ContextRecord->EFlags |= 0x100;
            t_watchStep = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_watchStep) {
        t_watchStep = false;
        if (t_watchWord && !t_watchOld && rd32(t_watchWord)) {                // set (it is cleared every frame; whoever sets it decides)
            static const uintptr_t mod = (uintptr_t)GetModuleHandleA(nullptr);
            fprintf(stderr, "[watch] f=%llu entry %u word %08X: 0 -> %08X at host +%llX, guest lr=%08X\n", (unsigned long long)gpu_frame_number(),
                    (t_watchWord - g_watchLo) / 156, t_watchWord, rd32(t_watchWord), (unsigned long long)(t_watchRip - mod), g_ctx ? (uint32_t)g_ctx->lr : 0);
        }
        watch_protect(PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void watch_arm(uint32_t lo, uint32_t hi, uint32_t field) {
    if (g_watchHi) return;
    AddVectoredExceptionHandler(1, watch_handler);
    g_watchLo = lo; g_watchHi = hi; g_watchField = field;
    watch_protect(PAGE_READONLY);
    fprintf(stderr, "[watch] watching %08X-%08X, word +%u of each 156-byte entry\n", lo, hi, field);
}

// ASURA_PHYSWATCH=<physical hex>: write-protect that physical page in all three windows (A0, C0, E0 views of the same memory) from
// the first frame, and log the first writes: which window and address, the host code address and the guest lr.
static uint32_t g_pwPhys = 0;
static thread_local int t_pwStep = 0;
static void pw_protect(DWORD prot) {
    DWORD old;
    for (uint32_t w : { 0xA0000000u, 0xC0000000u, 0xE0000000u }) VirtualProtect(g_base + w + g_pwPhys, 0x1000, prot, &old);
}
static LONG CALLBACK pw_handler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!g_pwPhys) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] == 1 && !t_pwStep) {
        const uint8_t* a = (const uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        for (uint32_t w : { 0xA0000000u, 0xC0000000u, 0xE0000000u }) {
            if (a >= g_base + w + g_pwPhys && a < g_base + w + g_pwPhys + 0x1000) {
                static int n = 0;
                static const uintptr_t mod = (uintptr_t)GetModuleHandleA(nullptr);
                if (n++ < 40) fprintf(stderr, "[physwatch] f=%llu write to %08X (window %08X) at host +%llX, guest lr=%08X\n", (unsigned long long)gpu_frame_number(),
                                      (uint32_t)(a - g_base), w, (unsigned long long)(ep->ContextRecord->Rip - mod), g_ctx ? (uint32_t)g_ctx->lr : 0);
                pw_protect(PAGE_READWRITE);
                ep->ContextRecord->EFlags |= 0x100;
                t_pwStep = 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_pwStep) { t_pwStep = 0; pw_protect(PAGE_READONLY); return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}
void physwatch_tick() {
    static bool armed = false;
    if (armed) return;
    armed = true;
    if (const char* e = getenv("ASURA_PHYSWATCH")) {
        g_pwPhys = (uint32_t)strtoul(e, nullptr, 16) & ~0xFFFu;
        AddVectoredExceptionHandler(1, pw_handler);
        pw_protect(PAGE_READONLY);
        fprintf(stderr, "[physwatch] watching physical %08X in A0, C0 and E0\n", g_pwPhys);
    }
}

// ASURA_DISTDUMP="first,last" (frames): sub_8273D208 is UE3's per-object, per-view distance cull. With r29 = the object's
// scene info and r31 = the view: squared distance from the view origin (+880) to the bounds box (+16 origin, +28 extent);
// culled when beyond min(+48 * c * c, view +208), c the float at 832AC300, or nearer than +44, or the +60 test. Logs the inputs.
static float rdf(uint32_t a) { uint32_t w = rd32(a); float f; memcpy(&f, &w, 4); return f; }
static thread_local uint32_t t_distPrim = 0;
PPC_FUNC_IMPL(__imp__sub_8273D208);
PPC_FUNC(sub_8273D208) {
    static long long first = -2, last = -2;
    if (first == -2) { first = last = -1; if (const char* e = getenv("ASURA_DISTDUMP")) sscanf(e, "%lld,%lld", &first, &last); }
    const long long f = (long long)gpu_frame_number();
    if (f >= first && f <= last && ctx.r4.u32) {
        const uint32_t prim = ctx.r4.u32, self = ctx.r3.u32, view = rd32(self + 80);
        const float ox = rdf(prim + 16), oy = rdf(prim + 20), oz = rdf(prim + 24), ex = rdf(prim + 28), ey = rdf(prim + 32), ez = rdf(prim + 36);
        const float vx = rdf(view + 880), vy = rdf(view + 884), vz = rdf(view + 888);
        auto axis = [](float v, float o, float e) { return v < o - e ? o - e - v : (v > o + e ? v - o - e : 0.0f); };
        const float dx = axis(vx, ox, ex), dy = axis(vy, oy, ey), dz = axis(vz, oz, ez), d2 = dx * dx + dy * dy + dz * dz;
        fprintf(stderr, "[dist] f=%lld prim=%08X origin=%g,%g,%g extent=%g,%g,%g view=%g,%g,%g dist=%g min+44=%g max+48=%g +60=%g c=%g view+208=%g flags+76=%u r5=%u\n",
                f, prim, ox, oy, oz, ex, ey, ez, vx, vy, vz, sqrtf(d2), rdf(prim + 44), rdf(prim + 48), rdf(prim + 60), rdf(0x832AC300), rdf(self + 208), rd32(prim + 76), ctx.r5.u32);
        t_distPrim = prim;
        __imp__sub_8273D208(ctx, base);
        t_distPrim = 0;
        return;
    }
    __imp__sub_8273D208(ctx, base);
}
// sub_82748850(viewState, bounds, view, ..., out) after the frustum test: nonzero culls the object (UE3's occlusion check).
PPC_FUNC_IMPL(__imp__sub_82748850);
PPC_FUNC(sub_82748850) {
    const uint32_t prim = t_distPrim, st = ctx.r3.u32, bounds = ctx.r4.u32;
    const double f1 = ctx.f1.f64;
    __imp__sub_82748850(ctx, base);
    if (prim) fprintf(stderr, "[dist]   prim=%08X occlusion test -> %u (state %08X, bounds %g,%g,%g / %g,%g,%g r=%g, f1=%g)\n", prim, ctx.r3.u32, st,
                      rdf(bounds), rdf(bounds + 4), rdf(bounds + 8), rdf(bounds + 12), rdf(bounds + 16), rdf(bounds + 20), rdf(bounds + 24), f1);
}
// D3D query GetData (sub_8299FAF8): out = sum over the query's sample blocks of (end ZPass A+B at +16/+20) - (begin at +48/+52),
// read byte-swapped; "not ready" while the ZPass words hold FFFFFEED. Logs the blocks while the dump is on.
PPC_FUNC_IMPL(__imp__sub_8299FAF8);
PPC_FUNC(sub_8299FAF8) {
    const uint32_t q = ctx.r3.u32, out = ctx.r4.u32;
    __imp__sub_8299FAF8(ctx, base);
    if (t_distPrim && rd32(q + 4) == 9) {
        const uint32_t n = rd32(q + 148);
        std::string s;
        for (uint32_t k = 0; k < n && k < 8; k++) {
            const uint32_t p = rd32(q + 28 + k * 4);
            uint32_t a = (p & 0x1FFFFFFFu) + ((((p >> 20) & 0xFFFu) + 512) & 0x1000u) + 0xC0000000u;
            char b[160]; snprintf(b, sizeof b, " [%08X->%08X: %08X %08X %08X %08X | %08X %08X]", p, a, rd32(a + 16), rd32(a + 20), rd32(a + 48), rd32(a + 52), rd32(a), rd32(a + 32));
            s += b;
        }
        fprintf(stderr, "[dist]   GetData q=%08X ret=%u out=%d blocks=%u%s\n", q, ctx.r3.u32, (int)rd32(out), n, s.c_str());
    }
}
// While the dump is on: which objects pass (reach sub_82740AB8), and what the frustum test sub_822C3450 returns for them.
PPC_FUNC_IMPL(__imp__sub_82740AB8);
PPC_FUNC(sub_82740AB8) {
    if (t_distPrim) fprintf(stderr, "[dist]   prim=%08X passes\n", t_distPrim);
    __imp__sub_82740AB8(ctx, base);
}
PPC_FUNC_IMPL(__imp__sub_822C3450);
PPC_FUNC(sub_822C3450) {
    const uint32_t prim = t_distPrim, frustum = ctx.r3.u32, box = ctx.r4.u32, sph = ctx.r5.u32;
    __imp__sub_822C3450(ctx, base);
    if (prim) {
        fprintf(stderr, "[dist]   prim=%08X frustum test -> %u (box %g,%g,%g / %g,%g,%g, arg %08X) frustum %08X\n", prim, ctx.r3.u32,
                rdf(box), rdf(box + 4), rdf(box + 8), rdf(box + 12), rdf(box + 16), rdf(box + 20), sph, frustum);
    }
}

// ASURA_CULLDUMP="first,last" (frames): sub_8271BCA8 decides per entry whether a scene object is drawn (the Gohma's entry stops
// being drawn as it moves away). Its entries are 64 bytes in the array at *(*this + 16352): object pointer at +0, three floats
// at +32/+36/+40 (skipped when all three square below a threshold), a flags word at +48. Logs them for those frames.
PPC_FUNC_IMPL(__imp__sub_8271BCA8);
PPC_FUNC(sub_8271BCA8) {
    static long long first = -2, last = -2;
    if (first == -2) { first = last = -1; if (const char* e = getenv("ASURA_CULLDUMP")) sscanf(e, "%lld,%lld", &first, &last); }
    const long long f = (long long)gpu_frame_number();
    if (f >= first && f <= last) {
        const uint32_t self = ctx.r3.u32, cont = rd32(self) + 16352, data = rd32(cont), num = rd32(cont + 4);
        uint32_t th = rd32(0x8207FB10 + 396); float thf; memcpy(&thf, &th, 4);
        static const bool kWatch = getenv("ASURA_CULLWATCH") != nullptr;
        if (kWatch && ctx.r4.u32 == 1 && ctx.r5.u32 == 1) {                    // the whole view-0 table, the slot this call reads
            const uint32_t vbase = rd32(self + 80), tab = vbase ? rd32(vbase + 2028) : 0;
            if (tab && num) watch_arm(tab, tab + num * 156, (ctx.r4.u32 + 1) * 28);
        }
        fprintf(stderr, "[cull] f=%lld this=%08X r4=%08X r5=%08X r6=%08X array=%08X num=%u threshold=%g\n", f, self, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, data, num, thf);
        for (uint32_t i = 0; i < num && i < 256 && data; i++) {
            const uint32_t e = data + i * 64, obj = rd32(e);
            if (obj < 0x40000000u || obj >= 0x7F000000u || IsBadReadPtr(g_base + obj, 360)) continue;
            float v[3]; for (int k = 0; k < 3; k++) { uint32_t w = rd32(e + 32 + k * 4); memcpy(&v[k], &w, 4); }
            // per view: the visibility table entry (156 bytes per object) the loop checks: its first word and the word at (r4+1)*28
            std::string vis;
            const uint32_t views = rd32(self + 84), vbase = rd32(self + 80);
            for (uint32_t j = 0; j < views && j < 4 && vbase; j++) {
                const uint32_t tab = rd32(vbase + j * 4272 + 2028);
                if (!tab) { vis += " v?=null"; continue; }
                const uint32_t ent = tab + i * 156;
                char b[48]; snprintf(b, sizeof b, " v%u=%08X/%08X", j, rd32(ent), rd32(ent + (ctx.r4.u32 + 1) * 28));
                vis += b;
            }
            fprintf(stderr, "[cull]   %u obj=%08X flags=%08X v=%g,%g,%g state=%u%s\n", i, obj, rd32(e + 48), v[0], v[1], v[2], g_base[obj + 352], vis.c_str());
        }
    }
    __imp__sub_8271BCA8(ctx, base);
}

// UE3 fatal error (appError path): prints the message and parks the thread with Sleep(INFINITE). Log it, always.
PPC_FUNC_IMPL(__imp__sub_8229BB28);
PPC_FUNC(sub_8229BB28) {
    fprintf(stderr, "[fatal] %s | %s (lr=%08X)\n", guest_text(ctx.r4.u32).c_str(), guest_text(ctx.r5.u32).c_str(), (uint32_t)ctx.lr);
    fflush(stderr);
    __imp__sub_8229BB28(ctx, base);
}

// ASURA_QTELOG=1: the natives behind QTEs ("Synchronic" cinematic tracks, BCPlayerInput's button bookkeeping, the input judge). Each
// logs this, its result word and the frame, but only when the result changes for that object, so per-frame polling stays readable.
// Native addresses come from the name table at 831FF598-831FF9DC (name, function pairs).
static bool qtelog_on() { static const bool on = getenv("ASURA_QTELOG") != nullptr; return on; }
static void qtelog(const char* name, uint32_t self, uint32_t result) {
    static std::mutex m; static std::map<std::pair<const char*, uint32_t>, uint32_t> last;
    std::lock_guard<std::mutex> g(m);
    auto key = std::make_pair(name, self);
    auto it = last.find(key);
    if (it != last.end() && it->second == result) return;
    last[key] = result;
    fprintf(stderr, "[qtelog] f=%llu %s this=%08X -> %08X\n", (unsigned long long)gpu_frame_number(), name, self, result);
}
#define QTE_HOOK(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { \
        const uint32_t self = ctx.r3.u32, res = ctx.r5.u32; \
        __imp__sub_##addr(ctx, base); \
        if (qtelog_on()) qtelog(label, self, res ? rd32(res) : 0xFFFFFFFFu); \
    }
QTE_HOOK(82950A10, "Synchronic.IsStartTrack")
QTE_HOOK(82950B50, "Synchronic.IsEndTrack")
QTE_HOOK(82950C80, "Synchronic.IsInputOk")
QTE_HOOK(82950DA8, "Synchronic.IsInputStart")
QTE_HOOK(82950E60, "Synchronic.IsInputEnd")
PPC_FUNC_IMPL(__imp__sub_824BA210);
static std::atomic<uint32_t> g_qteJudge{0};
PPC_FUNC(sub_824BA210) {                                                    // ABCInputJudgeSystem::execCheckMissInput: calls vtable+888
    const uint32_t self = ctx.r3.u32, res = ctx.r5.u32;
    __imp__sub_824BA210(ctx, base);
    g_qteJudge = self;
    if (!qtelog_on()) return;
    static bool namesScanned = false;
    if (!namesScanned) {                                                    // once: where the controller key names sit, and the words before them
        namesScanned = true;
        static const char kKey[] = "XboxTypeS_";
        for (uint32_t pg = 0x40000000u; pg < 0x50000000u; pg += 0x10000) {
            if (IsBadReadPtr(g_base + pg, 0x10000)) continue;
            const uint8_t* p = g_base + pg;
            for (uint32_t o = 16; o + 40 < 0x10000; o++) {
                if (p[o] != 'X' || memcmp(p + o, kKey, 10)) continue;
                char name[48] = {}; for (int k = 0; k < 47 && p[o + k] >= 32 && p[o + k] < 127; k++) name[k] = (char)p[o + k];
                fprintf(stderr, "[qtename] %08X %-32s before: %08X %08X %08X %08X\n", pg + o, name, rd32(pg + o - 16), rd32(pg + o - 12), rd32(pg + o - 8), rd32(pg + o - 4));
            }
        }
    }
    // Each frame of a QTE: when the judge's prompt lists change, dump their elements and each element's input list (+0x20 {data, count, max}).
    static uint32_t lastSig = 0;
    uint32_t sig = 0;
    for (uint32_t arr : { 0x1C4u, 0x1E4u }) sig = sig * 31 + rd32(self + arr) * 7 + rd32(self + arr + 4);
    if (sig == lastSig) return;
    lastSig = sig;
    for (uint32_t arr : { 0x1C4u, 0x1E4u }) {
        const uint32_t data = rd32(self + arr), n = rd32(self + arr + 4);
        fprintf(stderr, "[qtelist] f=%llu list +%X: %u entries at %08X\n", (unsigned long long)gpu_frame_number(), arr, n, data);
        for (uint32_t i = 0; i < n && i < 8 && data && !IsBadReadPtr(g_base + data + i * 0x60, 0x60); i++) {
            std::string e = "[qtelist]   entry:"; char b[16];
            for (uint32_t o = 0; o < 0x60; o += 4) { snprintf(b, sizeof b, " %08X", rd32(data + i * 0x60 + o)); e += b; }
            fprintf(stderr, "%s\n", e.c_str());
            for (uint32_t ao : { 0x00u, 0x20u }) {                           // the element's two arrays {data, count, max}: 0x20 bytes per item shown
                const uint32_t in = rd32(data + i * 0x60 + ao), cnt = rd32(data + i * 0x60 + ao + 4);
                if (!in || !cnt || cnt > 8 || IsBadReadPtr(g_base + in, cnt * 0x20)) continue;
                std::string q = "[qtelist]     array +" + std::to_string(ao) + ":";
                for (uint32_t o = 0; o < cnt * 0x20; o += 4) { snprintf(b, sizeof b, " %08X", rd32(in + o)); q += b; }
                fprintf(stderr, "%s\n", q.c_str());
            }
        }
    }
}
// Called by the renderer when the on-screen QTE prompts change: dumps the judge object (ASURA_QTELOG), to find where it keeps the
// expected input and its result.
void qte_judge_dump(const char* why) {
    const uint32_t j = g_qteJudge.load();
    if (!qtelog_on() || !j) return;
    std::string s = "[qtejudge] f=" + std::to_string(gpu_frame_number()) + " " + why + " " ;
    char b[16];
    for (uint32_t o = 0; o < 0x600; o += 4) { snprintf(b, sizeof b, "%08X ", rd32(j + o)); s += b; }
    fprintf(stderr, "%s\n", s.c_str());
    for (uint32_t arr : { 0x19Cu, 0x1C4u, 0x1E4u }) {                       // TArrays {data, count, max}: their elements (0x60 bytes each shown)
        const uint32_t data = rd32(j + arr), n = rd32(j + arr + 4);
        if (!data || n == 0 || n > 16 || IsBadReadPtr(g_base + data, 0x60 * n)) continue;
        std::string e = "[qtejudge]   array +" + std::to_string(arr) + " (" + std::to_string(n) + "):";
        for (uint32_t o = 0; o < 0x60 * n && o < 0x200; o += 4) { snprintf(b, sizeof b, " %08X", rd32(data + o)); e += b; }
        fprintf(stderr, "%s\n", e.c_str());
        for (uint32_t po : { 0x00u, 0x20u }) {                               // the element's pointers: what they point at
            const uint32_t p = rd32(data + po);
            if (p < 0x40000000u || p >= 0x50000000u || IsBadReadPtr(g_base + p, 0x100)) continue;
            std::string q = "[qtejudge]     *+" + std::to_string(po) + " " + std::to_string(p) + ":";
            for (uint32_t o = 0; o < 0x100; o += 4) { snprintf(b, sizeof b, " %08X", rd32(p + o)); q += b; }
            fprintf(stderr, "%s\n", q.c_str());
        }
    }
}
QTE_HOOK(824B8218, "PlayerInput.UpdateInputList")
QTE_HOOK(82929340, "PlayerInput.UpdateButtonInputCondition")
QTE_HOOK(829293F8, "PlayerInput.SetPressedButton")
QTE_HOOK(82929488, "PlayerInput.SetRepeatButton")
QTE_HOOK(82929518, "PlayerInput.SetReleaseButton")
QTE_HOOK(829295A8, "PlayerInput.ResetRepeatButton")
