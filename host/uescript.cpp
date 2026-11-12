// UE3 script-event tracing (debugging aid). ASURA_SCRIPTLOG="first,last" (frames): log each script event native code sends
// (FindFunctionChecked by name, then ProcessEvent), with the object's name and class, between those frames.
// ASURA_SCRIPTLOGFILTER=<text>: only objects whose name, class or event contains it (case-insensitive).
#include "kernel.h"
#include <windows.h>
#include <string>
#include <mutex>
#include <map>
#include <cctype>
#include <cstdarg>
#include <vector>
#include <algorithm>

uint64_t gpu_frame_number();
static std::string S(const char* fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); return buf;
}

// The name table (GNames): an array of pointers to name entries {Index (idx << 1 | wide), HashNext, characters}. Found once by
// scanning for the entry of "None" (index 0) and then for an array whose first two pointers lead to entries 0 and 1.
static uint32_t g_names = 0, g_namesCount = 0, g_nameHdr = 8;
static bool readable(uint32_t a, uint32_t n) {                            // committed and readable, checked without touching it
    if (a < 0x10000 || (uint64_t)a + n > 0x100000000ull) return false;
    for (uint64_t p = a & ~0xFFFull; p < (uint64_t)a + n; p += 0x1000) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    }
    return true;
}
// Scans of one committed region. The game frees memory on other threads meanwhile, and the host's vectored crash handler would
// end the process on a fault before any __except, so the region is copied with ReadProcessMemory (which just fails) in chunks.
static uint32_t g_noneAt[256], g_noneHdr[256], g_noneN = 0;
static std::vector<uint8_t> g_chunk(0x10000 + 16);
static bool copy_chunk(uint64_t at, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), g_base + at, g_chunk.data(), n, &got) && got == n;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void scan_none(uint64_t off, uint64_t size) {
    for (uint64_t c = 0; c + 32 < size && g_noneN < 256; c += 0x10000) {
        const size_t n = (size_t)std::min<uint64_t>(0x10000 + 16, size - c);
        if (!copy_chunk(off + c, n)) continue;
        const uint8_t* p = g_chunk.data();
        for (size_t o = 16; o + 5 <= n && g_noneN < 256; o += 4) {
            if (p[o] != 'N' || memcmp(p + o, "None", 5)) continue;
            for (uint32_t hdr = 8; hdr <= 16; hdr += 4)
                if (be32(p + o - hdr) == 0) { g_noneAt[g_noneN] = (uint32_t)(off + c + o - hdr); g_noneHdr[g_noneN++] = hdr; break; }
        }
    }
}
static bool entry_is(uint32_t e, uint32_t index) {                          // a name entry with this index (wide bit ignored)
    uint8_t b[4]; SIZE_T got = 0;
    if (e < 0x10000 || !ReadProcessMemory(GetCurrentProcess(), g_base + e, b, 4, &got) || got != 4) return false;
    return (be32(b) & ~1u) == index << 1;
}
static uint32_t scan_ref(uint64_t off, uint64_t size, uint32_t e0, uint32_t& refs) {   // the address of an array starting {e0, entry 1}, or 0
    for (uint64_t c = 0; c + 8 <= size; c += 0x10000) {
        const size_t n = (size_t)std::min<uint64_t>(0x10000 + 4, size - c);
        if (!copy_chunk(off + c, n)) continue;
        const uint8_t* p = g_chunk.data();
        for (size_t o = 0; o + 8 <= n; o += 4) {
            if (be32(p + o) != e0) continue;
            refs++;
            if (entry_is(be32(p + o + 4), 1)) return (uint32_t)(off + c + o);
        }
    }
    return 0;
}
template <class F> static void each_region(F f) {
    for (uint64_t off = 0x10000; off < 0x100000000ull; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + off, &mi, sizeof mi)) break;
        const uint64_t size = mi.RegionSize - (uint64_t)((g_base + off) - (uint8_t*)mi.BaseAddress);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && !f(off, size)) return;
        off += size;
    }
}
static bool find_names() {
    static long long lastTry = -1000;                                       // the table may not exist yet early on: retry every 300 frames
    if (g_names) return true;
    const long long now = (long long)gpu_frame_number();
    if (now - lastTry < 300) return false;
    lastTry = now;
    g_noneN = 0;
    each_region([](uint64_t off, uint64_t size) { scan_none(off, size); return g_noneN < 256; });
    for (uint32_t k = 0; k < g_noneN && !g_names; k++) {
        const uint32_t e0 = g_noneAt[k];
        uint32_t refs = 0;
        each_region([&](uint64_t off, uint64_t size) { g_names = scan_ref(off, size, e0, refs); return g_names == 0; });
        if (!g_names) { fprintf(stderr, "[scriptlog] None candidate %08X (header %u): %u references, none an array\n", e0, g_noneHdr[k], refs); continue; }
        g_nameHdr = g_noneHdr[k];
        for (g_namesCount = 0; g_namesCount < 1000000; g_namesCount++) {
            uint8_t b[4]; SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), g_base + g_names + g_namesCount * 4, b, 4, &got) || got != 4) break;
            const uint32_t e = be32(b);
            if (e && !entry_is(e, g_namesCount)) break;
        }
        fprintf(stderr, "[scriptlog] name table at %08X, %u names (None at %08X, header %u)\n", g_names, g_namesCount, e0, g_nameHdr);
    }
    if (!g_names) fprintf(stderr, "[scriptlog] name table not found (%u None candidates)\n", g_noneN);
    return g_names != 0;
}
// The table's own TArray {data, count, max} (a global in the image): the array is reallocated as names are added, so lookups go
// through it rather than through the data address found by the scan.
static uint32_t g_namesArr = 0;
static void find_names_header() {
    g_namesArr = 0;
    each_region([](uint64_t off, uint64_t size) {
        for (uint64_t c = 0; c + 12 <= size && !g_namesArr; c += 0x10000) {
            const size_t n = (size_t)std::min<uint64_t>(0x10000 + 8, size - c);
            if (!copy_chunk(off + c, n)) continue;
            const uint8_t* p = g_chunk.data();
            for (size_t o = 0; o + 12 <= n; o += 4)
                if (be32(p + o) == g_names && be32(p + o + 4) >= g_namesCount && be32(p + o + 8) >= be32(p + o + 4) && be32(p + o + 8) < 4000000) { g_namesArr = (uint32_t)(off + c + o); break; }
        }
        return g_namesArr == 0;
    });
    fprintf(stderr, "[scriptlog] name table header %s %08X\n", g_namesArr ? "at" : "not found; data", g_namesArr ? g_namesArr : g_names);
}
std::string ue_name(uint32_t idx, uint32_t number = 0) {
    const std::string none = "#" + std::to_string(idx);
    if (!find_names()) return none;
    static bool hdrTried = false;
    if (!hdrTried) { hdrTried = true; find_names_header(); }
    const uint32_t data = g_namesArr ? rd32(g_namesArr) : g_names, count = g_namesArr ? rd32(g_namesArr + 4) : g_namesCount;
    if (idx >= count && !g_namesArr) { g_names = 0; hdrTried = false; }         // found before the table grew and no header: look again later
    if (idx >= count || !readable(data + idx * 4, 4)) return none;
    const uint32_t e = rd32(data + idx * 4);
    if (!e || !readable(e, g_nameHdr + 2)) return none;
    std::string s;
    const bool wide = rd32(e) & 1;
    for (uint32_t i = 0; i < 96; i++) {
        const uint32_t a = wide ? e + g_nameHdr + i * 2 : e + g_nameHdr + i;
        if ((a & 0xFFF) == 0 && !readable(a, 2)) break;
        const uint32_t c = wide ? rd16(a) : g_base[a];
        if (!c) break;
        s += c < 0x80 ? (char)c : '?';
    }
    if (number) s += "_" + std::to_string(number - 1);
    return s;
}
// UObject: Name (FName: index, number) at +0x2C, Class at +0x34 (checked against the judge: BCInputJudgeSystem).
static const uint32_t kNameOff = getenv("ASURA_UENAMEOFF") ? strtoul(getenv("ASURA_UENAMEOFF"), nullptr, 16) : 0x2C;
static const uint32_t kClassOff = getenv("ASURA_UECLASSOFF") ? strtoul(getenv("ASURA_UECLASSOFF"), nullptr, 16) : 0x34;
std::string ue_obj_name(uint32_t o) {
    if (!readable(o, 0x40)) return "(null)";
    return ue_name(rd32(o + kNameOff), rd32(o + kNameOff + 4));
}
std::string ue_class_name(uint32_t o) {
    if (!readable(o, 0x40)) return "(null)";
    return ue_obj_name(rd32(o + kClassOff));
}

static bool g_logOn = false;
static long long g_first = -1, g_last = -1;
static std::string g_filter;
static bool scriptlog_active() {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = getenv("ASURA_SCRIPTLOG")) { g_logOn = true; sscanf(e, "%lld,%lld", &g_first, &g_last); }
        if (const char* f = getenv("ASURA_SCRIPTLOGFILTER")) { g_filter = f; for (auto& c : g_filter) c = (char)tolower(c); }
    }
    if (!g_logOn) return false;
    const long long f = (long long)gpu_frame_number();
    return f >= g_first && (g_last < 0 || f <= g_last);
}
static bool matches(const std::string& s) {
    if (g_filter.empty()) return true;
    std::string l = s; for (auto& c : l) c = (char)tolower(c);
    return l.find(g_filter) != std::string::npos;
}

// A wide string at p (an FString's characters), if it looks like printable text of 2+ characters.
static std::string wide_at(uint32_t p) {
    if (!readable(p, 4)) return "";
    std::string s;
    for (uint32_t i = 0; i < 64; i++) {
        if (!readable(p + i * 2, 2)) return "";
        const uint16_t c = rd16(p + i * 2);
        if (!c) break;
        if (c < 32 || c > 126) return "";
        s += (char)c;
    }
    return s.size() >= 2 ? s : "";
}
// ASURA_SCRIPTDUMP=<class text>: for objects of that class, once per object and event, dump the object's first 0x200 bytes and
// every FString reachable through one pointer (an array's elements): "+offset -> +k "text"", to read output link names.
static void script_dump(uint32_t self, const std::string& cls, const std::string& ev, const char* when) {
    static const char* kWant = getenv("ASURA_SCRIPTDUMP");
    if (!kWant || cls.find(kWant) == std::string::npos) return;
    static std::map<std::string, int> seen;
    if (seen[S("%08X %s %s", self, ev.c_str(), when)]++) return;
    std::string s = S("[sdump] f=%llu %s %08X %s %s:", (unsigned long long)gpu_frame_number(), cls.c_str(), self, ev.c_str(), when);
    for (uint32_t o = 0; o < 0x200; o += 4) s += S("%s%08X", o % 32 ? " " : S("\n[sdump]   +%03X ", o).c_str(), rd32(self + o));
    fprintf(stderr, "%s\n", s.c_str());
    for (uint32_t o = 0; o < 0x200; o += 4) {
        const uint32_t p = rd32(self + o);
        if (p < 0x10000 || !readable(p, 0x100)) continue;
        for (uint32_t k = 0; k < 0x180; k += 4) {
            const uint32_t q = rd32(p + k);
            const std::string t = wide_at(q);
            if (!t.empty()) fprintf(stderr, "[sdump]   +%03X -> %08X +%03X \"%s\"\n", o, p, k, t.c_str());
        }
        const std::string d = wide_at(p);
        if (!d.empty()) fprintf(stderr, "[sdump]   +%03X \"%s\"\n", o, d.c_str());
        const uint32_t n = rd32(self + o + 4), mx = rd32(self + o + 8);    // a TArray {data, count, max}: its elements
        if (n >= 1 && n <= 16 && mx >= n && mx <= 64 && readable(p, n * 0x40)) {
            std::string a = S("[sdump]   array +%03X (%u):", o, n);
            for (uint32_t k = 0; k < n * 0x40; k += 4) a += S("%s%08X", k % 0x40 ? " " : "\n[sdump]     ", rd32(p + k));
            fprintf(stderr, "%s\n", a.c_str());
        }
    }
}

// ASURA_QTESKIP=1 (Story Mode): every QTE succeeds at once, with no prompt and no prompt sound. A QTE is a Kismet action,
// BCSeqAct_InputDivergence (vtable 821BED08): Activated fires its "Input Start" output; its native update (sub_82981178) then sends
// the script event Update each frame (the ProcessEvent call returning to 82981250). The first Update registers the buttons with the
// judge and opens the prompt; on success Update fires the "Success" output, clears bit 0x10000000 and sets 0x20000000 | 0x01000000
// (done) in the flags word at +0xC4, and the native update then ends the action (Deactivated, StopShowButtonAll). Here Update is not
// run at all: "Success" is fired and the flags set as on success. Output links: TArray at +0x78 of 0x2C-byte entries {Links TArray,
// LinkDesc FString at +0x0C, bits at +0x18 (top bit: impulse)}; "Success" is found by its description.
static bool qte_skip(uint32_t self, uint32_t lr) {
    static const bool kOn = [] { const char* e = getenv("ASURA_QTESKIP"); return e && *e && strcmp(e, "0") != 0; }();
    if (!kOn || lr != 0x82981250u || !readable(self, 0x100) || rd32(self) != 0x821BED08u) return false;
    const uint32_t links = rd32(self + 0x78), n = rd32(self + 0x7C);
    bool fired = false;
    for (uint32_t i = 0; i < n && i < 8 && readable(links + i * 0x2C, 0x2C); i++) {
        const uint32_t e = links + i * 0x2C;
        if (wide_at(rd32(e + 0x0C)) != "Success") continue;
        wr32(e + 0x18, rd32(e + 0x18) | 0x80000000u);
        fired = true;
    }
    if (!fired) { fprintf(stderr, "[qteskip] f=%llu %08X has no Success output: left to run\n", (unsigned long long)gpu_frame_number(), self); return false; }
    wr32(self + 0xC4, (rd32(self + 0xC4) & ~0x10000000u) | 0x21000000u);
    fprintf(stderr, "[qteskip] f=%llu %08X: Success\n", (unsigned long long)gpu_frame_number(), self);
    return true;
}

// UObject::ProcessEvent(UFunction*, void* parms, void* result): every script event sent from native code.
PPC_FUNC_IMPL(__imp__sub_82260F60);
PPC_FUNC(sub_82260F60) {
    if (qte_skip(ctx.r3.u32, (uint32_t)ctx.lr)) return;
    if (!scriptlog_active()) { __imp__sub_82260F60(ctx, base); return; }
    const uint32_t self = ctx.r3.u32, fn = ctx.r4.u32, parms = ctx.r5.u32;
    const std::string obj = ue_obj_name(self), cls = ue_class_name(self), ev = ue_obj_name(fn);
    if (matches(obj) || matches(cls) || matches(ev)) {
        std::string p;
        if (readable(parms, 16)) p = S(" parms %08X %08X %08X %08X", rd32(parms), rd32(parms + 4), rd32(parms + 8), rd32(parms + 12));
        fprintf(stderr, "[script] f=%llu %s (%s %08X) <- %s%s lr=%08X\n", (unsigned long long)gpu_frame_number(), obj.c_str(), cls.c_str(), self, ev.c_str(), p.c_str(), (uint32_t)ctx.lr);
    }
    script_dump(self, cls, ev, "before");
    __imp__sub_82260F60(ctx, base);
    script_dump(self, cls, ev, "after");
}
