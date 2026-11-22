// UE3 script-event tracing (debugging aid). ASURA_SCRIPTLOG="first,last" (frames): log each script event native code sends
// (FindFunctionChecked by name, then ProcessEvent), with the object's name and class, between those frames.
// ASURA_SCRIPTLOGFILTER=<text>: only objects whose name, class or event contains it (case-insensitive).
#include "kernel.h"
#include <windows.h>
#include <string>
#include <mutex>
#include <map>
#include <atomic>
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
    static bool hdrTried = false;
    uint32_t e = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!find_names()) return none;
        if (!hdrTried) { hdrTried = true; find_names_header(); }
        const uint32_t data = g_namesArr ? rd32(g_namesArr) : g_names, count = g_namesArr ? rd32(g_namesArr + 4) : g_namesCount;
        e = idx < count && readable(data + idx * 4, 4) ? rd32(data + idx * 4) : 0;
        // Every entry stores its own index: one that doesn't match (or an index past the end) means the table was reallocated since
        // it was found (the game grows it at level loads; the old copy is freed and reused). Find it again, at most every 300 frames.
        if (e && readable(e, g_nameHdr + 2) && entry_is(e, idx)) break;
        e = 0;
        static long long lastRescan = -1000;
        const long long now = (long long)gpu_frame_number();
        if (attempt || now - lastRescan < 300) return none;
        lastRescan = now;
        g_names = 0; g_namesArr = 0; hdrTried = false;
        fprintf(stderr, "[scriptlog] name %u not where the table says (moved?): looking for the table again\n", idx);
    }
    if (!e) return none;
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
static std::vector<std::pair<long long, long long>> g_ranges;     // ASURA_SCRIPTLOG="first,last[;first,last...]" (last -1: no end)
static std::string g_filter;
static bool scriptlog_active() {
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = getenv("ASURA_SCRIPTLOG")) {
            g_logOn = true;
            for (const char* p = e; p && *p; ) {
                long long a = -1, b = -1;
                if (sscanf(p, "%lld,%lld", &a, &b) >= 1) g_ranges.push_back({a, b});
                p = strchr(p, ';'); if (p) p++;
            }
        }
        if (const char* f = getenv("ASURA_SCRIPTLOGFILTER")) { g_filter = f; for (auto& c : g_filter) c = (char)tolower(c); }
    }
    if (!g_logOn) return false;
    const long long f = (long long)gpu_frame_number();
    for (auto& r : g_ranges) if (f >= r.first && (r.second < 0 || f <= r.second)) return true;
    return false;
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

// ASURA_CLASSDUMP=<class text> (debug): once, for the first object of a matching class seen in an event, dump its class object and
// walk what looks like the class's field list: any pointer in the class to an object whose class ends in "Property" / "Function",
// followed through the dword at +0x3C (UField::Next), printing each field's name, kind and its dwords +0x3C..+0x7C.
static void class_dump(uint32_t self, const std::string& cls) {
    static const char* kWant = getenv("ASURA_CLASSDUMP");
    static std::map<std::string, bool> done;                                  // a comma-separated list: one dump per entry
    if (!kWant) return;
    std::string hit;
    for (const char* p = kWant; *p;) { const char* q = strchr(p, ','); std::string w(p, q ? q - p : strlen(p)); if (!w.empty() && cls.find(w) != std::string::npos) { hit = w; break; } if (!q) break; p = q + 1; }
    if (hit.empty() || done[hit]) return;
    done[hit] = true;
    auto ends = [](const std::string& s, const char* t) { const size_t n = strlen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; };
    for (uint32_t c = rd32(self + kClassOff), depth = 0; c && readable(c, 0x100) && depth < 8; depth++) {
        std::string s = S("[cdump] class %s %08X (class %s):", ue_obj_name(c).c_str(), c, ue_class_name(c).c_str());
        for (uint32_t o = 0; o < 0x100; o += 4) s += S("%s%08X", o % 32 ? " " : S("\n[cdump]   +%03X ", o).c_str(), rd32(c + o));
        fprintf(stderr, "%s\n", s.c_str());
        uint32_t super = 0;
        for (uint32_t o = 0x3C; o < 0x100; o += 4) {
            const uint32_t p = rd32(c + o);
            if (!readable(p, 0x80)) continue;
            const std::string k = ue_class_name(p);
            if (k == "Class" && !super && p != c) { super = p; fprintf(stderr, "[cdump]   +%03X -> class %s %08X\n", o, ue_obj_name(p).c_str(), p); continue; }
            if (!ends(k, "Property") && !ends(k, "Function") && !ends(k, "Const") && !ends(k, "Enum") && !ends(k, "State") && !ends(k, "Struct")) continue;
            fprintf(stderr, "[cdump]   +%03X -> field list from %s %s\n", o, k.c_str(), ue_obj_name(p).c_str());
            for (uint32_t f = p, n = 0; f && readable(f, 0x80) && n < 400; f = rd32(f + 0x3C), n++) {
                std::string l = S("[cdump]     %08X %-16s %-28s", f, ue_class_name(f).c_str(), ue_obj_name(f).c_str());
                for (uint32_t q = 0x3C; q < 0x80; q += 4) l += S(" %08X", rd32(f + q));
                fprintf(stderr, "%s\n", l.c_str());
            }
        }
        c = super;
    }
}

// UE3 reflection, as laid out in this build (found with ASURA_CLASSDUMP): UField::Next at +0x3C; UStruct::SuperField at +0x40 and
// Children (the first field) at +0x44; UProperty::Offset at +0x54; UBoolProperty::BitMask at +0x64. ue_field_offset looks a
// property up by name through a class and its parents (cached per class); -1 if there is none. For a bool, mask gets its bit.
static int ue_field_offset(uint32_t cls, const char* name, uint32_t* mask = nullptr) {
    static std::map<std::pair<uint32_t, std::string>, std::pair<int, uint32_t>> cache;
    const auto key = std::make_pair(cls, std::string(name));
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::pair<int, uint32_t> r{-1, 0};
        for (uint32_t c = cls, depth = 0; c && readable(c, 0x48) && depth < 16 && r.first < 0; c = rd32(c + 0x40), depth++)
            for (uint32_t f = rd32(c + 0x44), n = 0; f && readable(f, 0x60) && n < 2000; f = rd32(f + 0x3C), n++)
                if (ue_obj_name(f) == name) { r = { (int)rd32(f + 0x54), rd32(f + 0x64) }; break; }
        if (r.first < 0 || r.first > 0x10000) r = {-1, 0};
        fprintf(stderr, "[ue] %s.%s at %s%X%s\n", ue_obj_name(cls).c_str(), name, r.first < 0 ? "(none) " : "+", r.first < 0 ? 0 : r.first,
                ue_class_name(cls) == "Class" && mask && r.first >= 0 ? S(" mask %08X", r.second).c_str() : "");
        it = cache.emplace(key, r).first;
    }
    if (mask) *mask = it->second.second;
    return it->second.first;
}

// Every property of an object with its value, by the class's own field lists (subclass first): ints, floats, bools, bytes, names,
// object references (by name), and arrays as {count}. Structs are shown as their first dwords. For reading an unknown object's state.
static std::vector<std::pair<std::string, std::string>> ue_props(uint32_t obj) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!readable(obj, 0x40)) return out;
    for (uint32_t c = rd32(obj + kClassOff), depth = 0; c && readable(c, 0x48) && depth < 6; c = rd32(c + 0x40), depth++) {
        const std::string cname = ue_obj_name(c);
        if (cname == "Object") break;
        for (uint32_t f = rd32(c + 0x44), k = 0; f && readable(f, 0x60) && k < 4000; f = rd32(f + 0x3C), k++) {
            const std::string kind = ue_class_name(f);
            if (kind.size() < 8 || kind.compare(kind.size() - 8, 8, "Property") != 0) continue;
            const uint32_t off = rd32(f + 0x54), size = rd32(f + 0x44);
            if (off > 0x4000 || !readable(obj + off, 4)) continue;
            const uint32_t v = rd32(obj + off);
            std::string val;
            if (kind == "IntProperty") val = S("%d", (int32_t)v);
            else if (kind == "FloatProperty") { float x; memcpy(&x, &v, 4); val = S("%g", x); }
            else if (kind == "BoolProperty") val = (v & rd32(f + 0x64)) ? "true" : "false";
            else if (kind == "ByteProperty") val = S("%u", g_base[obj + off]);
            else if (kind == "NameProperty") val = ue_name(v, rd32(obj + off + 4));
            else if (kind == "ObjectProperty" || kind == "ClassProperty" || kind == "ComponentProperty") val = v ? ue_obj_name(v) : "None";
            else if (kind == "ArrayProperty") {                                 // the count, and for short arrays each element's first 0x40 bytes
                const uint32_t cnt = rd32(obj + off + 4);
                val = S("{%u}", cnt);
                // An array of structs: its element size and the struct's fields (name@offset, bools with their mask). Inner property at
                // +0x64 (like StructProperty::Struct and BoolProperty::BitMask), its element size at +0x44.
                uint32_t stride = 0x40;
                const uint32_t inner = rd32(f + 0x64);
                if (readable(inner, 0x68) && ue_class_name(inner) == "StructProperty") {
                    const uint32_t es = rd32(inner + 0x44), st = rd32(inner + 0x64);
                    if (es >= 4 && es <= 0x200) stride = es;
                    std::string lay = S(" <%s %X:", readable(st, 0x48) ? ue_obj_name(st).c_str() : "?", stride);
                    for (uint32_t sf = readable(st, 0x48) ? rd32(st + 0x44) : 0, m = 0; sf && readable(sf, 0x68) && m < 64; sf = rd32(sf + 0x3C), m++) {
                        const std::string sk = ue_class_name(sf);
                        lay += S(" %s@%X", ue_obj_name(sf).c_str(), rd32(sf + 0x54));
                        if (sk == "BoolProperty") lay += S("/%08X", rd32(sf + 0x64));
                    }
                    val += lay + ">";
                }
                for (uint32_t e = 0; e < cnt && e < 4 && v && readable(v + e * stride, stride < 0x40 ? stride : 0x40); e++) {
                    val += S(" [%u:", e);
                    for (uint32_t q = 0; q < 0x40 && q < stride; q += 4) val += S(" %08X", rd32(v + e * stride + q));
                    val += "]";
                }
            }
            else if (kind == "StrProperty") val = "\"" + wide_at(v) + "\"";
            else { val = S("%08X", v); for (uint32_t q = 4; q < size && q < 0x40; q += 4) val += S(" %08X", rd32(obj + off + q)); }   // structs: all their dwords
            out.push_back({ cname + "." + ue_obj_name(f), val });
        }
    }
    return out;
}
void ue_dump_props(uint32_t obj, const char* tag) {
    if (!readable(obj, 0x40)) return;
    std::string out = S("[props] f=%llu %s %s (%s %08X):", (unsigned long long)gpu_frame_number(), tag, ue_obj_name(obj).c_str(), ue_class_name(obj).c_str(), obj);
    int n = 0;
    for (auto& kv : ue_props(obj)) out += S("%s%s=%s", n++ % 6 ? "  " : "\n[props]   ", kv.first.c_str(), kv.second.c_str());
    fprintf(stderr, "%s\n", out.c_str());
}
// ASURA_PROPWATCH=<class name>[,<class name>...] (debug): at each Tick of an object of that class, log the properties that changed since
// its previous Tick ([pwatch]). Slow (the whole property list each Tick): for finding which field means "open", "shown" and so on.
static void prop_watch(uint32_t self, const std::string& cls, const std::string& ev) {
    static const char* kWant = getenv("ASURA_PROPWATCH");
    if (!kWant || ev != "Tick") return;
    if (("," + std::string(kWant) + ",").find("," + cls + ",") == std::string::npos) return;
    static std::map<uint32_t, std::map<std::string, std::string>> last;
    auto& m = last[self];
    std::string out;
    for (auto& kv : ue_props(self)) {
        auto it = m.find(kv.first);
        if (it != m.end() && it->second != kv.second) out += S("  %s: %s -> %s", kv.first.c_str(), it->second.c_str(), kv.second.c_str());
        m[kv.first] = kv.second;
    }
    if (!out.empty()) fprintf(stderr, "[pwatch] f=%llu %s %08X:%s\n", (unsigned long long)gpu_frame_number(), cls.c_str(), self, out.c_str());
}

// Story Mode. Each piece is a switch the settings menu sets at run time (options.cpp); ASURA_INVINCIBLE / ASURA_QTESKIP set by hand win.
static std::atomic<int> g_invincible{-1}, g_qteSkip{-1};                       // -1: not set yet (read the environment)
static bool env_on(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0; }
bool story_invincible() { int v = g_invincible.load(); if (v < 0) { v = env_on("ASURA_INVINCIBLE"); g_invincible = v; } return v != 0; }
// In-combat "tug of war" prompts (mash B, rotate LS; a gold meter scrolls in from the top) are not QTE actions but
// BCSeqAct_AnimFadeByButtonRep: each press pushes an animation blend (BCAnim_FadeByButtonRep). Its native update sends the script
// event CheckActivate every frame while it runs (caller 82982600; logs\ep5_run3, episode 5 frames 6,745-6,848). Story Mode presses
// its buttons for the player while it is active (xam.cpp qte_apply, from the judge like ASURA_AUTOQTE) and the renderer hides
// the prompt icons meanwhile. Active = polled within the last 3 frames.
static std::atomic<uint64_t> g_mashSeen{0};
void story_mash_note() { g_mashSeen = gpu_frame_number() + 1; }
bool story_mash_active() { const uint64_t s = g_mashSeen.load(); return s && gpu_frame_number() + 1 <= s + 3; }
// For hiding its UI: also the next 45 frames, while the meter scrolls back out of the top of the screen.
bool story_mash_recent() { const uint64_t s = g_mashSeen.load(); return s && gpu_frame_number() + 1 <= s + 45; }
bool story_qteskip() { int v = g_qteSkip.load(); if (v < 0) { v = env_on("ASURA_QTESKIP"); g_qteSkip = v; } return v != 0; }
void story_set(bool invincible, bool qteskip) {
    if (!getenv("ASURA_INVINCIBLE")) g_invincible = invincible;
    if (!getenv("ASURA_QTESKIP")) g_qteSkip = qteskip;
}

// ASURA_INVINCIBLE=1 (Story Mode): the player takes no damage. The player is BCPawn_Player_asr (and the other BCPawn_Player
// classes); UE3's Pawn.Health (+0x2D8) and HealthMax (+0x2DC) are looked up by name. At every script event the player gets (Tick
// each frame, and the damage notifications) its Health is put back to HealthMax, and its Controller's bGodMode is set, which
// UE3's damage code checks (Pawn.InGodMode) before taking any health off.
// ASURA_HEALTHLOG=1 logs the player's Health whenever it changes (with or without invincibility).
static std::atomic<uint64_t> g_mantraFillFrom{0}, g_mantraFillUntil{0};  // Story Mode: fill the Burst gauge in these frames (qte_skip, Mantra gauge QTEs)
static void player_health(uint32_t self, const char* when) {
    static const bool kLog = env_on("ASURA_HEALTHLOG");
    const bool inv = story_invincible();
    static const bool kAutopilot = env_on("ASURA_AUTOPILOT");               // testing (xam.cpp): keep the Burst (Mantra) gauge full, so fights end in a BURST
    const bool fill = kAutopilot || (gpu_frame_number() >= g_mantraFillFrom.load() && gpu_frame_number() < g_mantraFillUntil.load());
    // ASURA_BURSTSTART=<percent> (testing, and the start of Story Mode's "Burst start" item): each new player pawn (every gameplay
    // segment spawns one) starts with the Burst gauge at least that full. Raised during the pawn's first 2 s, since the segment's
    // script may set the gauge itself just after the spawn.
    static const int kBurstStart = [] { const char* e = getenv("ASURA_BURSTSTART"); const int v = e ? atoi(e) : 0; return v < 0 ? 0 : v > 100 ? 100 : v; }();
    if (!inv && !kLog && !fill && !kBurstStart) return;
    const uint32_t cls = rd32(self + kClassOff);
    static std::map<uint32_t, bool> isPlayer;
    auto it = isPlayer.find(cls);
    if (it == isPlayer.end()) it = isPlayer.emplace(cls, ue_obj_name(cls).rfind("BCPawn_Player", 0) == 0).first;
    if (!it->second) return;
    if (kBurstStart) {
        static uint32_t pawn = 0; static uint64_t since = 0;
        const uint64_t fr = gpu_frame_number();
        if (self != pawn) { pawn = self; since = fr; }
        const int vOff = ue_field_offset(cls, "MantraGaugeVal"), xOff = ue_field_offset(cls, "MantraGaugeValMax");
        if (fr - since < 60 && vOff >= 0 && xOff >= 0 && readable(self + vOff, 4) && readable(self + xOff, 4)) {
            const uint32_t rv = rd32(self + vOff), rx = rd32(self + xOff);
            const bool isInt = rx < 0x10000u;                               // the maximum is 1000: as a float its bits are far above this
            float cur, mx; if (isInt) { cur = (float)(int32_t)rv; mx = (float)(int32_t)rx; } else { memcpy(&cur, &rv, 4); memcpy(&mx, &rx, 4); }
            const float want = mx * (float)kBurstStart / 100.0f;
            if (mx > 0 && cur < want) {
                uint32_t w; if (isInt) w = (uint32_t)(int32_t)want; else memcpy(&w, &want, 4);
                wr32(self + vOff, w);
                static uint32_t said = 0;
                if (said != self) { said = self; fprintf(stderr, "[story] f=%llu %s: Burst gauge set to %d%% (%.0f of %.0f)\n", (unsigned long long)fr, ue_obj_name(self).c_str(), kBurstStart, want, mx); }
            }
        }
        if (!inv && !kLog && !fill) return;
    }
    if (fill) {
        const int vOff = ue_field_offset(cls, "MantraGaugeVal"), xOff = ue_field_offset(cls, "MantraGaugeValMax");
        if (vOff >= 0 && xOff >= 0 && readable(self + vOff, 4) && readable(self + xOff, 4) && rd32(self + vOff) != rd32(self + xOff)) {
            if (kAutopilot) {
                wr32(self + vOff, rd32(self + xOff));                     // same type as the maximum, int or float
                static int nl = 0;
                if (nl++ < 20) fprintf(stderr, "[autopilot] f=%llu Burst gauge filled\n", (unsigned long long)gpu_frame_number());
            } else {
                // Story Mode's Mantra gauge QTE: filled at once at every poll, but only from 6 s after the QTE (qte_skip), so the punch
                // exchange plays out first (filled straight away it was over almost as soon as it began: the user, 2026-11-22). Raising
                // it a step a frame never filled it: the scene sets the gauge itself each frame (logs\rec_20261122_115831), so only a
                // full gauge at every poll gets through.
                const uint64_t fr = gpu_frame_number();
                static std::map<uint32_t, uint64_t> saidOf;
                if (fr - saidOf[self] > 90) {
                    uint32_t rv = rd32(self + vOff); float cur; memcpy(&cur, &rv, 4);
                    fprintf(stderr, "[qteskip] f=%llu %s: Burst gauge filled (was %.1f)\n", (unsigned long long)fr, ue_obj_name(self).c_str(), cur);
                    saidOf[self] = fr;
                }
                wr32(self + vOff, rd32(self + xOff));
            }
        }
        if (!inv && !kLog) return;
    }
    const int hOff = ue_field_offset(cls, "Health"), mOff = ue_field_offset(cls, "HealthMax");
    if (hOff < 0 || mOff < 0 || !readable(self + mOff, 4)) return;
    const int32_t h = (int32_t)rd32(self + hOff), hm = (int32_t)rd32(self + mOff);
    static std::map<uint32_t, int32_t> last;
    if (kLog && last[self] != h) fprintf(stderr, "[health] f=%llu %s %08X %s: Health %d (was %d) of %d\n", (unsigned long long)gpu_frame_number(),
                                         ue_obj_name(self).c_str(), self, when, h, last[self], hm);
    last[self] = h;
    {   // ASURA_HEALTHLOG=2: also every 300 frames, the player's Health line and all its properties (slow: a diagnostic replay only)
        static const bool kFull = []{ const char* e = getenv("ASURA_HEALTHLOG"); return e && atoi(e) >= 2; }();
        static uint64_t next = 0;
        const uint64_t fr = gpu_frame_number();
        if (kFull && fr >= next) {
            next = fr + 300;
            fprintf(stderr, "[health] f=%llu %s (%s) %08X: Health %d of %d\n", (unsigned long long)fr, ue_obj_name(self).c_str(), ue_obj_name(cls).c_str(), self, h, hm);
            ue_dump_props(self, "player");
        }
    }
    if (!inv) return;
    if (hm > 0 && h > 0 && h < hm) { wr32(self + hOff, (uint32_t)hm); last[self] = hm; }
    const int cOff = ue_field_offset(cls, "Controller");
    const uint32_t ctl = cOff >= 0 ? rd32(self + cOff) : 0;
    if (ctl && readable(ctl, 0x40)) {
        uint32_t mask = 0;
        const int gOff = ue_field_offset(rd32(ctl + kClassOff), "bGodMode", &mask);
        if (gOff >= 0 && mask && readable(ctl + gOff, 4) && !(rd32(ctl + gOff) & mask)) {
            wr32(ctl + gOff, rd32(ctl + gOff) | mask);
            fprintf(stderr, "[invincible] f=%llu %s: bGodMode set\n", (unsigned long long)gpu_frame_number(), ue_obj_name(ctl).c_str());
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
// ASURA_ENUMDUMP=1 (debug): at the first QTE Update, print the enum behind BCButtonInfo.InputButton (the judge's input codes) with
// every name, by walking InputButtons (ArrayProperty) -> Inner (StructProperty) -> Struct -> its InputButton (ByteProperty) -> Enum.
// The pointer each step follows is the first dword after UProperty's own fields that points at an object of the expected kind.
static void button_enum_dump(uint32_t self) {
    static bool done = false;
    if (done || !getenv("ASURA_ENUMDUMP")) return;
    done = true;
    auto find_field = [](uint32_t strct, const char* name) -> uint32_t {
        for (uint32_t c = strct, d = 0; c && readable(c, 0x48) && d < 16; c = rd32(c + 0x40), d++)
            for (uint32_t f = rd32(c + 0x44), n = 0; f && readable(f, 0x60) && n < 2000; f = rd32(f + 0x3C), n++)
                if (ue_obj_name(f) == name) return f;
        return 0;
    };
    auto follow = [](uint32_t prop, const char* kind, const char* name) -> uint32_t {   // a pointer in +0x40..+0x80 to that kind (and name)
        for (uint32_t o = 0x40; o < 0x80; o += 4) {
            const uint32_t p = rd32(prop + o);
            if (p == prop || !readable(p, 0x48) || ue_class_name(p) != kind || (name && ue_obj_name(p) != name)) continue;
            fprintf(stderr, "[enum] %s +%02X -> %s %s\n", ue_obj_name(prop).c_str(), o, kind, ue_obj_name(p).c_str());
            return p;
        }
        fprintf(stderr, "[enum] %s: no %s found\n", ue_obj_name(prop).c_str(), kind);
        return 0;
    };
    const uint32_t arr = find_field(rd32(self + kClassOff), "InputButtons");
    const uint32_t inner = arr ? follow(arr, "StructProperty", "InputButtons") : 0;   // an array's Inner has the array's name
    const uint32_t st = inner ? follow(inner, "ScriptStruct", nullptr) : 0;
    const uint32_t ib = st ? find_field(st, "InputButton") : 0;
    const uint32_t en = ib ? follow(ib, "Enum", nullptr) : 0;
    if (!en) return;
    for (uint32_t o = 0x40; o < 0x60; o += 4) {                         // UEnum::Names, a TArray<FName> {data, count, max}
        const uint32_t d = rd32(en + o), n = rd32(en + o + 4), mx = rd32(en + o + 8);
        if (n < 2 || n > 256 || mx < n || !readable(d, n * 8)) continue;
        for (uint32_t i = 0; i < n; i++) fprintf(stderr, "[enum] %s %u = %s\n", ue_obj_name(en).c_str(), i, ue_name(rd32(d + i * 8), rd32(d + i * 8 + 4)).c_str());
        return;
    }
    fprintf(stderr, "[enum] %s: names not found\n", ue_obj_name(en).c_str());
}

// A QTE Story Mode leaves to the player (below) was updated in the last 0.5 s: xam.cpp then presses no combat prompt for it
static std::atomic<uint64_t> g_manualMs{0};
bool story_manual_recent() { const uint64_t t = g_manualMs.load(); return t && GetTickCount64() - t < 500; }
// Episode 22's X/B choice that Story Mode answers (below) was updated in the last 0.3 s: the renderer hides its prompt
static std::atomic<uint64_t> g_choiceMs{0};
bool story_choice_active() { const uint64_t t = g_choiceMs.load(); return t && GetTickCount64() - t < 300; }
bool story_choice_recent1s() { const uint64_t t = g_choiceMs.load(); return t && GetTickCount64() - t < 1000; }   // xenos.cpp: its pictures
bool story_choice_recent() { const uint64_t t = g_choiceMs.load(); return t && GetTickCount64() - t < 3000; }   // xma.cpp: the chime's mute
static bool qte_skip(uint32_t self, uint32_t lr) {
    if (lr == 0x82981250u && readable(self, 0x120)) button_enum_dump(self);
    if (lr != 0x82981250u || !story_qteskip() || !readable(self, 0x120)) return false;   // any action run by this native update (subclasses too)
    // Succeed only after about the time a player takes, so cinematics that expect the input window don't run ahead and then hold
    // (the B mash at ~12,776 in episode 1). +0xDC: the time limit in seconds when the QTE has one (0 otherwise; a no-input QTE
    // with 0.866 there ended after 26 frames); +0x118: presses needed (1, or 15/30 for the mashes). Wait half the limit (0.73 s,
    // the window measured for a QTE with no limit, when unset); for a mash, about 0.12 s a press (the auto-answer's pace), at
    // most half the limit. Until then the script Update is not run, so no prompt opens.
    // Timed in real seconds, the same at any frame rate: each Update adds the wall time since the action's previous Update, capped
    // at 0.1 s so a hitch doesn't count in full. A gap over 2 s (a pause menu, or the action run again later) starts the wait over.
    {   // Left to the player: an action with no "Success" output is a free choice, not a QTE to pass (episode 10's hot springs:
        // Drink / Chug / Splash / Stand up; the user's run logs\rec_20261115_154808, frame 4,889). The skip used to hold its Update
        // back for the wait, then find no Success, run one Update and start a new wait, so the game read the pad one frame in twelve.
        const uint32_t links = rd32(self + 0x78), n = rd32(self + 0x7C);
        bool hasSuccess = false;
        std::string outs;
        for (uint32_t i = 0; i < n && i < 8 && readable(links + i * 0x2C, 0x2C); i++) {
            const std::string d = wide_at(rd32(links + i * 0x2C + 0x0C));
            if (d == "Success") { hasSuccess = true; break; }
            outs += (outs.empty() ? "" : ", ") + d;
        }
        if (!hasSuccess) {
            static std::map<uint32_t, uint64_t> said;
            const uint64_t f = gpu_frame_number();
            if (!said.count(self) || f - said[self] > 300) fprintf(stderr, "[qteskip] f=%llu %08X %s: left to the player (no Success output: %s)\n", (unsigned long long)f, self, ue_obj_name(self).c_str(), outs.c_str());
            said[self] = f;
            // Except episode 22's X/B prompt (BCSeqAct_ButtonDivergence, outputs Shot / Assult): Story Mode mashes X and B
            // alternately (the user's call, 2026-11-22) from 0.3 s after the choice opens while it stays open
            if (ue_obj_name(self).rfind("BCSeqAct_ButtonDivergence", 0) == 0 && outs.find("Shot") != std::string::npos && outs.find("Assult") != std::string::npos) {
                struct Choice { uint64_t first = 0, seen = 0, pressed = 0; uint16_t button = 0; };
                static std::map<uint32_t, Choice> choices;
                static bool nextB = false;
                const uint64_t now = GetTickCount64();
                Choice& ch = choices[self];
                if (!ch.seen || now - ch.seen > 2000) { ch = Choice(); ch.first = now; ch.button = nextB ? 0x2000 : 0x4000; nextB = !nextB; }
                ch.seen = now;
                g_choiceMs = now;                                           // the renderer hides its prompt (xenos.cpp, story_choice_active)
                // X and B mashed alternately, about 10 presses a second (50 ms down): one press a second, one button per choice, let each
                // choice time out into its failure (the user's run, logs\rec_20261122_124809 f=25583-25904: failure emblem and sound)
                if (now - ch.first >= 300 && now - ch.pressed >= 100) {
                    ch.pressed = now;
                    void input_press_at(uint16_t buttons, uint32_t delayMs, uint32_t holdMs);
                    input_press_at(ch.button, 0, 50);
                    static int nl = 0;
                    if (nl++ < 40) fprintf(stderr, "[qteskip] f=%llu %08X %s: choice pressed %s\n", (unsigned long long)f, self, ue_obj_name(self).c_str(), ch.button == 0x2000 ? "B (Assult)" : "X (Shot)");
                    ch.button = ch.button == 0x2000 ? 0x4000 : 0x2000;
                }
                return false;
            }
            g_manualMs = GetTickCount64();
            return false;
        }
    }
    // Left to the player: the "Shut Wyzen Up" (episodes 4 and 5) and "Shut Augus Up" (episode 11) prompts (the user's call, 2026-11-14).
    // They are the only QTEs seen with no time limit (TimeLimit 0: the game waits for the press) whose buttons are all hidden
    // (InputButtons, an array of BCButtonInfo {InputButton@0, ButtonName@4, IconType@C, pos@10, PosType@1C, bHidden@20 (80000000),
    // Tag@24, bShow / bSuccess@2C}, 0x30 bytes each; the game draws its own prompt for them). Checked against episode 5's 16 QTEs
    // (logs\ep5_qprops2) and the first two of episode 11 (logs\st11c). Wyzen's also has no failure sound and FinEventTrigger 0.5,
    // Augus's has neither, so those are not the mark.
    {
        struct Offs { int tl = -1, btn = -1; };
        static std::map<uint32_t, Offs> offs;
        const uint32_t cls = rd32(self + kClassOff);
        auto o = offs.find(cls);
        if (o == offs.end()) {
            Offs v; v.tl = ue_field_offset(cls, "TimeLimit"); v.btn = ue_field_offset(cls, "InputButtons");
            o = offs.emplace(cls, v).first;
        }
        const Offs& v = o->second;
        if (v.tl > 0 && v.btn > 0 && rd32(self + v.tl) == 0) {
            const uint32_t b = rd32(self + v.btn), nb = rd32(self + v.btn + 4);
            bool allHidden = nb > 0 && nb <= 8 && readable(b, nb * 0x30);
            for (uint32_t i = 0; allHidden && i < nb; i++) allHidden = (rd32(b + i * 0x30 + 0x20) & 0x80000000u) != 0;
            if (allHidden) {
                static std::map<uint32_t, uint64_t> said;
                const uint64_t f = gpu_frame_number();
                if (!said.count(self) || f - said[self] > 300) fprintf(stderr, "[qteskip] f=%llu %08X %s: left to the player (no time limit, buttons hidden)\n", (unsigned long long)f, self, ue_obj_name(self).c_str());
                said[self] = f; g_manualMs = GetTickCount64();
                return false;
            }
        }
    }
    struct Wait { LARGE_INTEGER first, last; double elapsed; };
    static std::map<uint32_t, Wait> waits;
    static const double kQpf = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    auto it = waits.find(self);
    if (it == waits.end()) {
        it = waits.emplace(self, Wait{now, now, 0.0}).first;
        static const bool kProps = getenv("ASURA_QTEPROPS") != nullptr;   // debug: every property of each QTE when it starts
        if (kProps) ue_dump_props(self, "qte");
    }
    else {
        const double gap = (now.QuadPart - it->second.last.QuadPart) / kQpf;
        if (gap > 2.0) { it->second.first = now; it->second.elapsed = 0.0; }
        else it->second.elapsed += gap < 0.1 ? gap : 0.1;
        it->second.last = now;
    }
    float limit; { const uint32_t u = rd32(self + 0xDC); memcpy(&limit, &u, 4); }
    if (!(limit > 0.05f && limit < 60.f)) limit = 0.73f;
    const uint32_t presses = rd32(self + 0x118);
    float wait = limit * 0.5f;
    if (presses > 1 && presses <= 100) { const float m = presses * 0.12f; if (!rd32(self + 0xDC) || m < wait) wait = m; }
    static const float kScale = [] { const char* e = getenv("ASURA_QTESKIPWAIT"); return e ? (float)atof(e) : 1.f; }();   // 0: at once
    if (it->second.elapsed < wait * kScale) return true;
    // An input replay (ASURA_INPUTPLAY, recorded with the QTEs answered at once) leaves the wait out, so its gameplay input still
    // lines up with the game.
    void input_clock_pause(uint64_t ms);
    input_clock_pause((uint64_t)((now.QuadPart - it->second.first.QuadPart) / kQpf * 1000.0));
    waits.erase(it);
    const uint32_t links = rd32(self + 0x78), n = rd32(self + 0x7C);
    bool fired = false;
    for (uint32_t i = 0; i < n && i < 8 && readable(links + i * 0x2C, 0x2C); i++) {
        const uint32_t e = links + i * 0x2C;
        if (wide_at(rd32(e + 0x0C)) != "Success") continue;
        wr32(e + 0x18, rd32(e + 0x18) | 0x80000000u);
        fired = true;
    }
    if (!fired) return false;                                              // not reached: checked above
    // The flags as a real success leaves them (+0xC4 18800000 -> 2C800000 in a trace): not the "done" bit 0x01000000, which the
    // native update sets itself after its end-of-QTE work (vtable+400/+404, sub_82980ED8). Setting it here skipped that work and
    // left effects frozen after the QTE (the fire splatter after the B mash, the arm effect on Asura in the last stick QTE).
    wr32(self + 0xC4, (rd32(self + 0xC4) & ~0x10000000u) | 0x24800000u);
    {   // No success sound: episode 22's boss QTEs (the hidden Devanagari medallions, xenos.cpp) still played the QTE success chime when
        // passed (the user, 2026-11-22). BCSeqAct_InputDivergence.bUseSuccessSE is cleared first; other QTEs were already silent.
        static std::map<uint32_t, std::pair<int, uint32_t>> se;
        const uint32_t cls = rd32(self + kClassOff);
        auto it = se.find(cls);
        if (it == se.end()) { uint32_t m = 0; const int o = ue_field_offset(cls, "bUseSuccessSE", &m); it = se.emplace(cls, std::make_pair(o, m)).first; }
        if (it->second.first > 0 && it->second.second) wr32(self + it->second.first, rd32(self + it->second.first) & ~it->second.second);
    }
    fprintf(stderr, "[qteskip] f=%llu %08X %s: Success after %.2f s (limit %.2f, presses %u)\n", (unsigned long long)gpu_frame_number(), self, ue_obj_name(self).c_str(), wait * kScale, limit, presses);
    // A Mantra gauge QTE (mash B, each press adds MantraAmount to the Burst gauge; done at SuccessfulMantraGauge): the scene after it
    // goes on only once the gauge is full and the player BURSTs, so passing the action alone left episode 22's punch exchange with
    // Chakravartin repeating for ever (the user's run, logs\rec_20261122_110058 f=26059 on). The player's gauge is filled as well
    // (player_health, after 5 or 6 s of the exchange); the BURST itself stays the player's, like every BURST.
    if (ue_obj_name(self).find("MantraGauge") != std::string::npos) {
        // 5 s for the first exchange of a fight and 6 s after it (the user's call, 2026-11-22: episode 22 has two, ~30 s apart); a
        // new fight is one more than 2 minutes after the last exchange. Then the gauge is kept full for 2 s (player_health).
        static uint64_t lastAt = 0;
        const uint64_t fr = gpu_frame_number();
        const uint64_t wait = (!lastAt || fr - lastAt > 3600) ? 150 : 180;
        lastAt = fr;
        g_mantraFillFrom = fr + wait;
        g_mantraFillUntil = fr + wait + 60;
        fprintf(stderr, "[qteskip] f=%llu Mantra gauge QTE: filling the Burst gauge\n", (unsigned long long)gpu_frame_number());
    }
    return true;
}

// ASURA_QTETRACE=1 (debug): for each QTE action's script Update, log the full object once, then every dword in +0x80..+0x200 that
// changed since the previous Update (hex and as float), to find the time-limit and timer fields.
static void qte_trace(uint32_t self, uint32_t lr, uint32_t parms, const char* when) {
    static const bool kOn = [] { const char* e = getenv("ASURA_QTETRACE"); return e && *e && strcmp(e, "0") != 0; }();
    if (!kOn || lr != 0x82981250u || !readable(self, 0x200)) return;
    static std::map<uint32_t, std::vector<uint32_t>> last;
    auto& v = last[self];
    const unsigned long long f = (unsigned long long)gpu_frame_number();
    auto flt = [](uint32_t u) { float x; memcpy(&x, &u, 4); return x; };
    if (v.empty()) {
        v.resize(0x80);
        std::string s = S("[qtetrace] f=%llu %08X first %s (dt %g):", f, self, when, readable(parms, 4) ? flt(rd32(parms)) : 0.f);
        for (uint32_t o = 0; o < 0x200; o += 4) { v[o / 4] = rd32(self + o); s += S("%s%08X", o % 32 ? " " : S("\n[qtetrace]   +%03X ", o).c_str(), v[o / 4]); }
        fprintf(stderr, "%s\n", s.c_str());
        return;
    }
    std::string s;
    for (uint32_t o = 0x80; o < 0x200; o += 4) {
        const uint32_t u = rd32(self + o);
        if (u != v[o / 4]) { s += S(" +%03X %08X->%08X (%g)", o, v[o / 4], u, flt(u)); v[o / 4] = u; }
    }
    const uint32_t links = rd32(self + 0x78), n = rd32(self + 0x7C);                // outputs being fired
    for (uint32_t i = 0; i < n && i < 8 && readable(links + i * 0x2C, 0x2C); i++)
        if (rd32(links + i * 0x2C + 0x18) & 0x80000000u) s += S(" fires \"%s\"", wide_at(rd32(links + i * 0x2C + 0x0C)).c_str());
    if (!s.empty()) fprintf(stderr, "[qtetrace] f=%llu %08X %s:%s\n", f, self, when, s.c_str());
}

// A script function of a class (or its parents) by name, or 0 (the same field lists as properties: functions are UFields too).
static uint32_t ue_find_function(uint32_t cls, const char* name) {
    static std::map<std::pair<uint32_t, std::string>, uint32_t> cache;
    const auto key = std::make_pair(cls, std::string(name));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    uint32_t r = 0;
    for (uint32_t c = cls, depth = 0; c && readable(c, 0x48) && depth < 16 && !r; c = rd32(c + 0x40), depth++)
        for (uint32_t f = rd32(c + 0x44), n = 0; f && readable(f, 0x60) && n < 4000; f = rd32(f + 0x3C), n++)
            if (ue_class_name(f) == "Function" && ue_obj_name(f) == name) { r = f; break; }
    fprintf(stderr, "[ue] %s.%s() %s %08X\n", ue_obj_name(cls).c_str(), name, r ? "at" : "not found", r);
    cache[key] = r;
    return r;
}
// Calling a script function on an object from the host (from inside the ProcessEvent hook, on the game thread): ue_find_function finds
// it, ue_call_event sends it like an event with a zeroed parameter block. Tried for the Synchronic Y icon that stayed on screen
// (BCGFxUI_Synchronic.EndSynchronic, BCGFxMovieButton.FinishAll: neither cleared it; the renderer hides it, xenos.cpp); kept for later.
PPC_FUNC_IMPL(__imp__sub_82260F60);
static void ue_call_event(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t fn) {
    PPCContext c = ctx;
    const uint32_t sp = ctx.r1.u32;
    const uint32_t parms = (sp - 0x400) & ~15u;                            // a zeroed parameter block below the caller's frame
    memset(g_base + parms, 0, 0x200);
    c.r1.u64 = (sp - 0x800) & ~15u;                                         // the callee's frames go below the block
    c.r3.u64 = obj; c.r4.u64 = fn; c.r5.u64 = parms; c.r6.u64 = 0; c.lr = 0;
    __imp__sub_82260F60(c, base);
}

// UObject::ProcessEvent(UFunction*, void* parms, void* result): every script event sent from native code.
PPC_FUNC(sub_82260F60) {
    const uint32_t tSelf = ctx.r3.u32, tLr = (uint32_t)ctx.lr, tParms = ctx.r5.u32;
    qte_trace(tSelf, tLr, tParms, "before");
    if (qte_skip(ctx.r3.u32, (uint32_t)ctx.lr)) return;
    if (tLr == 0x82982600u && story_qteskip()) {                            // BCSeqAct_AnimFadeByButtonRep.CheckActivate: see story_mash_active
        story_mash_note();
        static const bool kProps = getenv("ASURA_QTEPROPS") != nullptr;     // debug: the tug of war's properties when it starts
        static std::map<uint32_t, uint64_t> dumped;
        const uint64_t f = gpu_frame_number();
        if (kProps && (!dumped.count(tSelf) || f - dumped[tSelf] > 300)) ue_dump_props(tSelf, "mash");
        if (kProps) dumped[tSelf] = f;
    }
    uint32_t mashFull = 0; float mashTop = 2.0f;                                                 // a tug-of-war node's ParamForFade to set to 2 after this event
    // Story Mode's tug of war: the meter is the blend node's ParamForFade (BCAnim_FadeByButtonRep, a BCAnim_FadeByParam; one node per
    // character), which the node's CalcParam (sent from 82947280 each frame) raises by AdditionWeightByPush (0.08) a press and lets
    // decay without them; the tug of war is won when it reaches the success child's value (2; it starts at StartPoint, 0.2; logs\st5g).
    // Pressing the buttons for the player (as first done, xam.cpp) played every press's sound, and a rotation has to turn the right
    // way. So nothing is pressed: before each CalcParam the value is raised by 0.4 a second of real time (about 5 s from 0.2 to 2, a
    // player's pace), never lowered. ASURA_MASHPRESS=1 presses instead (xam.cpp). ASURA_QTEPROPS=1 dumps the node every 10 frames.
    if (tLr == 0x82947280u && story_qteskip() && story_mash_recent()) {
        static const bool kProps = getenv("ASURA_QTEPROPS") != nullptr;
        static const bool kPress = env_on("ASURA_MASHPRESS");
        static std::map<uint32_t, bool> isNode;                             // class object -> BCAnim_FadeByButtonRep
        const uint32_t cls = rd32(tSelf + kClassOff);
        auto ni = isNode.find(cls);
        if (ni == isNode.end()) ni = isNode.emplace(cls, ue_obj_name(cls) == "BCAnim_FadeByButtonRep").first;
        if (ni->second) {
            static std::map<uint32_t, uint64_t> last;
            const uint64_t f = gpu_frame_number();
            if (kProps && f >= last[tSelf] + 10) { last[tSelf] = f; ue_dump_props(tSelf, "mashnode"); }
            const int po = ue_field_offset(cls, "ParamForFade");
            if (!kPress && story_mash_active() && po > 0) {
                struct Fill { float v; LARGE_INTEGER t; };
                static std::map<uint32_t, Fill> fills;
                static const double kQpf = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                float cur; { const uint32_t u = rd32(tSelf + po); memcpy(&cur, &u, 4); }
                auto it = fills.find(tSelf);
                if (it == fills.end() || (now.QuadPart - it->second.t.QuadPart) / kQpf > 1.0) {
                    it = fills.insert_or_assign(tSelf, Fill{ cur, now }).first;
                    fprintf(stderr, "[story] f=%llu tug of war: filling %s from %.2f, nothing pressed\n", (unsigned long long)f, ue_obj_name(tSelf).c_str(), cur);
                }
                double dt = (now.QuadPart - it->second.t.QuadPart) / kQpf;
                if (dt > 0.1) dt = 0.1;
                it->second.t = now;
                // The top: the meter's step count, BCAnim_FadeByParam.Constraints.Num() (2 in episodes 5 and 11 and most of 22; episode
                // 22's last tug of war has 4 steps, 0 to 3, and stuck at the old fixed 2.0 for ever: logs\rec_20261122_122628 f=28300)
                float top = 2.0f;
                {
                    static std::map<uint32_t, int> cOff;
                    auto ci = cOff.find(cls);
                    if (ci == cOff.end()) ci = cOff.emplace(cls, ue_field_offset(cls, "Constraints")).first;
                    if (ci->second > 0) { const uint32_t n = rd32(tSelf + ci->second + 4); if (n >= 2 && n <= 8) top = (float)n; }
                }
                it->second.v = std::max(it->second.v, cur) + (float)(0.4 * dt);
                if (it->second.v > top) it->second.v = top;
                uint32_t u; memcpy(&u, &it->second.v, 4); wr32(tSelf + po, u);
                if (it->second.v >= top) { mashFull = tSelf + po; mashTop = top; }          // full: also written after the script (it holds the value under 2 without a press)
            }
        }
    }
    struct HealthAfter { uint32_t self; ~HealthAfter() { player_health(self, "after"); } } healthAfter{tSelf};   // also on every return path
    struct MashFull { uint32_t a; float top; ~MashFull() { if (a) { uint32_t u; memcpy(&u, &top, 4); wr32(a, u); } } } mashFullAfter{mashFull, mashTop};   // the top, after the script
    // The Scaleform movie Story Mode looks after, by class (cached per class object, so this costs a lookup per event, not a name)
    static std::map<uint32_t, int> kinds;                                   // 2 BCGFxUI_Advertise, 0 anything else
    const uint32_t tCls = readable(tSelf, 0x40) ? rd32(tSelf + kClassOff) : 0;
    auto kit = kinds.find(tCls);
    if (kit == kinds.end()) { const std::string n = ue_obj_name(tCls); kit = kinds.emplace(tCls, n == "BCGFxUI_Advertise" ? 2 : 0).first; }
    // Story Mode: the help popups (BCGFxUI_Advertise with bUsePause: the game waits for A, such as episode 3's Synchronic tutorial)
    // are closed as soon as they take input: A is pressed once bInputNext is set (the "A Close" hint, about a second after opening;
    // an A before that is ignored, and setting bInputNext early didn't help: logs\endsync2).
    if (kit->second == 2 && story_qteskip()) {
        static uint32_t answered = 0;                                       // the popup (object) already answered while it stays open
        uint32_t mPause = 0, mNext = 0;
        const int oPause = ue_field_offset(tCls, "bUsePause", &mPause), oNext = ue_field_offset(tCls, "bInputNext", &mNext);
        if (oPause >= 0 && oNext >= 0 && mPause && mNext) {
            const bool open = (rd32(tSelf + oPause) & mPause) != 0;
            if (!open) answered = 0;
            else if (answered != tSelf && (rd32(tSelf + oNext) & mNext)) {
                answered = tSelf;
                void input_press_at(uint16_t buttons, uint32_t delayMs, uint32_t holdMs);
                input_press_at(0x1000, 0, 100);                             // A
                fprintf(stderr, "[story] f=%llu help popup %s closed (A)\n", (unsigned long long)gpu_frame_number(), ue_obj_name(tSelf).c_str());
            }
        }
    }
    player_health(tSelf, "before");
    static const bool kClassDump = getenv("ASURA_CLASSDUMP") != nullptr;
    if (kClassDump) class_dump(tSelf, ue_class_name(tSelf));
    {   // ASURA_PROPDUMP=<class name>: the property values of the first object of that class (exact name) seen in an event, once
        static const char* kProp = getenv("ASURA_PROPDUMP");
        static bool done = false;
        if (kProp && !done && ue_class_name(tSelf) == kProp) { done = true; ue_dump_props(tSelf, "first event"); }
    }
    {   // ASURA_PROPWATCH (debug) and the Story Mode Synchronic UI cleanup: both look at Tick events of particular classes
        static const bool kWatch = getenv("ASURA_PROPWATCH") != nullptr;
        if (kWatch) prop_watch(tSelf, ue_class_name(tSelf), ue_obj_name(ctx.r4.u32));
    }
    static const bool kTrace = getenv("ASURA_QTETRACE") != nullptr;
    if (!scriptlog_active()) { __imp__sub_82260F60(ctx, base); if (kTrace) qte_trace(tSelf, tLr, tParms, "after"); return; }
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
    if (kTrace) qte_trace(tSelf, tLr, tParms, "after");
}

// A property's offset in an object, looked up by name through its class (hooks.cpp); -1 if the class has none.
int ue_object_field(uint32_t obj, const char* name) { return readable(obj, 0x40) ? ue_field_offset(rd32(obj + kClassOff), name) : -1; }
