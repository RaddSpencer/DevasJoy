// Asura's Wrath host: guest memory, the program image, the function table, imports, and the guest main thread.
//
// Guest memory map:
//   40000000-6FFFFFFF  virtual allocations (NtAllocateVirtualMemory, thread stacks and PCRs, pools)
//   7F000000           imported variables, the XEX header copy and the module's loader entry
//   7FC80000           GPU registers (memory-mapped)
//   82000000           the image, then the indirect-call table (8 bytes per code word)
//   A0000000-BFFFFFFF  physical memory, aliased at C0000000 and E0000000 like the console
#include "kernel.h"
#include "shaderindex.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <vector>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

uint8_t* g_base = nullptr;
thread_local PPCContext* g_ctx = nullptr;
bool g_logging = false;
extern uint32_t g_xexHeaderGuest, g_exeModule, g_executionInfoGuest;
void register_file_imports();
void register_string_imports();
void gpu_init_mmio();
void monitor_start();
void register_xma_imports();

void guest_fp_init(PPCContext& ctx) { ctx.fpscr.loadFromHost(); }

static constexpr uint32_t kKernelData = 0x7F000000, kXexHeader = 0x7F010000, kLdrEntry = 0x7F030000;

typedef void* (WINAPI* PFN_VirtualAlloc2_)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef void* (WINAPI* PFN_MapViewOfFile3_)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
// 4 GB of guest space; the windows at A0/C0/E0000000 are views of one 512 MB section (from the BCR2 host)
static uint8_t* reserve_guest_memory() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto va2 = kb ? (PFN_VirtualAlloc2_)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    auto map3 = kb ? (PFN_MapViewOfFile3_)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
    if (!va2 || !map3) { fprintf(stderr, "VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803 or later needed)\n"); exit(1); }
    HANDLE proc = GetCurrentProcess();
    uint8_t* b = (uint8_t*)va2(proc, nullptr, 0x100000000ull, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!b) { fprintf(stderr, "could not reserve 4 GB: %lu\n", GetLastError()); exit(1); }
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x20000000, nullptr);
    const uint64_t cuts[3] = { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull };
    uint64_t prev = 0;
    for (uint64_t c : cuts) { VirtualFree(b + prev, (SIZE_T)(c - prev), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER); prev = c; }
    for (uint64_t w : cuts)
        if (!map3(sec, proc, b + w, 0, 0x20000000, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) { fprintf(stderr, "map %08llX failed: %lu\n", w, GetLastError()); exit(1); }
    if (!va2(proc, b, 0xA0000000ull, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0)) { fprintf(stderr, "low reserve failed: %lu\n", GetLastError()); exit(1); }
    return b;
}
static void commit(uint32_t a, size_t n) {
    if (!VirtualAlloc(g_base + a, n, MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "commit %08X+%zX failed: %lu\n", a, n, GetLastError()); exit(1); }
}

static void guest_backtrace(uint32_t sp) {
    fprintf(stderr, "    guest stack:");
    for (int k = 0; k < 16 && sp >= 0x40000000u && sp < 0x80000000u; k++) {
        uint32_t next = rd32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        fprintf(stderr, " %08X", rd32(next - 8));
        sp = next;
    }
    fprintf(stderr, "\n");
}
static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code >= 0xC000008Du && code <= 0xC0000093u) {   // floating-point traps: mask them and go on
        ep->ContextRecord->MxCsr = (ep->ContextRecord->MxCsr | 0x1F80u) & ~0x3Fu;
        ep->ContextRecord->FltSave.MxCsr = ep->ContextRecord->MxCsr;
        ep->ContextRecord->FltSave.ControlWord |= 0x3F;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C || code == 0x406D1388 || code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n*** EXCEPTION %08lX at host +%llX thread %lu\n", code,
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(nullptr)), GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        uint64_t a = ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "    %s guest %08llX\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from", (unsigned long long)(a - (uint64_t)g_base));
    }
    if (g_ctx) {
        PPCContext& c = *g_ctx;
        fprintf(stderr, "    lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r11=%08X r30=%08X r31=%08X\n",
                (uint32_t)c.lr, c.ctr.u32, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r11.u32, c.r30.u32, c.r31.u32);
        guest_backtrace(c.r1.u32);
        // the memory the non-volatile registers point at, which usually holds the objects the crashing code was working on
        const PPCRegister* regs[] = { &c.r3, &c.r4, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31 };
        const char* names[] = { "r3", "r4", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31" };
        for (int i = 0; i < 10; i++) {
            uint32_t a = regs[i]->u32 & ~3u;
            if (a < 0x10000 || IsBadReadPtr(g_base + a, 64)) continue;
            fprintf(stderr, "    [%s=%08X]", names[i], regs[i]->u32);
            for (int k = 0; k < 12; k++) fprintf(stderr, " %08X", rd32(a + k * 4));
            fprintf(stderr, "\n");
        }
    }
    if (const char* d = getenv("ASURA_CRASHDUMP")) {   // ASURA_CRASHDUMP=address,bytes (hex): dump that range too
        uint32_t a = 0, n = 0;
        if (sscanf(d, "%x,%x", &a, &n) == 2)
            for (uint32_t o = 0; o < n; o += 32) {
                if (IsBadReadPtr(g_base + a + o, 32)) break;
                fprintf(stderr, "    %08X:", a + o);
                for (int k = 0; k < 8; k++) fprintf(stderr, " %08X", rd32(a + o + k * 4));
                fprintf(stderr, "\n");
            }
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void UnresolvedIndirect(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "\n*** indirect call to %08X, which has no recompiled function (lr=%08X r3=%08X)\n", ctx.ctr.u32, (uint32_t)ctx.lr, ctx.r3.u32);
    guest_backtrace(ctx.r1.u32);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
}

static std::vector<char> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    // The exe is a windowed (GUI) program, so no console window opens when the game is started. Run from a terminal, it prints into
    // that terminal (the tool modes below report there); ASURA_CONSOLE=1 opens a console of its own. With --log, the log goes to a file.
    if (getenv("ASURA_CONSOLE") ? AllocConsole() : AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr);
    }
    if (argc >= 7 && !strcmp(argv[1], "--decode-dxt1")) {                     // --decode-dxt1 <raw file> <w> <h> <pitch texels> <tiled 0/1> [out.bmp]: big-endian 8in16 DXT1, as the console stores it
        std::ifstream f(argv[2], std::ios::binary); std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
        const uint32_t w = atoi(argv[3]), h = atoi(argv[4]), pitch = atoi(argv[5]); const bool tiled = atoi(argv[6]) != 0;
        const uint32_t bw = w / 4, bh = h / 4, pb = std::max(pitch / 4, bw), pa = (pb + 31) & ~31u;
        auto tiled2d = [](uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t l2) {
            uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6, inner = (((y >> 1) & 7) << 3) | (x & 7), oib = (outer | inner) << l2;
            uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1), ylsb = y & 1;
            return (ylsb << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12); };
        std::vector<uint8_t> px((size_t)w * h * 3, 0);
        for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
            const uint32_t a = tiled ? tiled2d(bx, by, pa, 3) : (by * pb + bx) * 8;
            if (a + 8 > raw.size()) continue;
            uint8_t b[8]; for (int k = 0; k < 8; k += 2) { b[k] = raw[a + k + 1]; b[k + 1] = raw[a + k]; }   // 8-in-16 swap
            const uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            uint8_t c[4][3];
            auto ex = [](uint16_t v, uint8_t* o) { o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; };
            ex(c0, c[0]); ex(c1, c[1]);
            for (int k = 0; k < 3; k++) { c[2][k] = c0 > c1 ? (2 * c[0][k] + c[1][k]) / 3 : (c[0][k] + c[1][k]) / 2; c[3][k] = c0 > c1 ? (c[0][k] + 2 * c[1][k]) / 3 : 0; }
            const uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            for (int i = 0; i < 16; i++) { const uint8_t* cc = c[(bits >> (2 * i)) & 3]; uint8_t* d = &px[((size_t)(by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 3]; d[0] = cc[2]; d[1] = cc[1]; d[2] = cc[0]; }
        }
        const std::string out = argc >= 8 ? argv[7] : std::string(argv[2]) + ".bmp";
        if (FILE* o = fopen(out.c_str(), "wb")) {
            const uint32_t rowB = (w * 3 + 3) & ~3u, img = rowB * h; uint8_t hd[54] = { 'B', 'M' };
            auto p32 = [&](int at, uint32_t v) { memcpy(hd + at, &v, 4); };
            p32(2, 54 + img); p32(10, 54); p32(14, 40); p32(18, w); p32(22, (uint32_t)(-(int32_t)h)); hd[26] = 1; hd[28] = 24; p32(34, img);
            fwrite(hd, 1, 54, o); std::vector<uint8_t> row(rowB, 0);
            for (uint32_t y = 0; y < h; y++) { memcpy(row.data(), &px[(size_t)y * w * 3], w * 3); fwrite(row.data(), 1, rowB, o); }
            fclose(o);
        }
        printf("wrote %s\n", out.c_str());
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--probe-lzx")) {                       // --probe-lzx <package> <start> <length> (hex): where LZX streams start, and recorded shaders in them
        std::vector<uint8_t> d;
        if (!ue3_unpack_file(argv[2], d)) { std::ifstream f(argv[2], std::ios::binary); d.assign(std::istreambuf_iterator<char>(f), {}); }
        const size_t s = strtoull(argv[3], nullptr, 16), len = strtoull(argv[4], nullptr, 16);
        std::vector<std::vector<uint8_t>> pats;                               // first 32 bytes of each recorded shader
        { std::ifstream m(data_root() + "\\cache\\shader_manifest.txt"); std::string line;
          while (std::getline(m, line)) {
              if (line.empty() || (line[0] != 'V' && line[0] != 'P')) continue;
              unsigned pg; size_t n; int used = 0; if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2 || n < 8) continue;
              std::vector<uint8_t> pat; const char* p = line.c_str() + 1 + used; unsigned v; int adv;
              for (int k = 0; k < 8 && sscanf(p, " %x%n", &v, &adv) == 1; k++) { pat.push_back(v >> 24); pat.push_back(v >> 16); pat.push_back(v >> 8); pat.push_back(v); p += adv; }
              pats.push_back(pat);
          } }
        std::vector<uint8_t> out; int streams = 0;
        for (size_t o = s; o < s + len && o + 2 < d.size(); o++) {
            if (d[o] != 0xFF && xmem_lzx_probe(&d[o], d.size() - o, 0x8000, out) < 0x8000) continue;   // a short stream starts with an FF frame
            const size_t got = xmem_lzx_probe(&d[o], d.size() - o, 0x40000, out);
            int conts = 0; for (size_t k = 0; k + 4 < out.size(); k++) if (out[k] == 0x10 && out[k + 1] == 0x2A && out[k + 2] == 0x11 && out[k + 3] <= 1) conts++;
            int hits = 0;
            if (conts) for (auto& pt : pats) if (std::search(out.begin(), out.end(), pt.begin(), pt.end()) != out.end()) hits++;
            if (!conts) continue;                                              // most "streams" are noise that happens to decode
            printf("stream at %zX (+%zX in the object): %zu bytes, %d containers, %d recorded shaders; 16 bytes before:", o, o - s, got, conts, hits);
            for (size_t k = o >= 16 ? o - 16 : 0; k < o; k++) printf(" %02X", d[k]);
            printf("\n");
            if (++streams >= 30) break;
        }
        printf("done\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--pkg-info")) {                        // --pkg-info <package> [class]: summary and exports (of that class) of a UE3 package
        std::vector<uint8_t> d;
        if (!ue3_unpack_file(argv[2], d)) { std::ifstream f(argv[2], std::ios::binary); d.assign(std::istreambuf_iterator<char>(f), {}); }
        size_t at = 0; bool bad = false;
        auto u32 = [&]() -> uint32_t { if (at + 4 > d.size()) { bad = true; return 0; } uint32_t v = (uint32_t)d[at] << 24 | (uint32_t)d[at + 1] << 16 | (uint32_t)d[at + 2] << 8 | d[at + 3]; at += 4; return v; };
        auto str = [&]() -> std::string { int32_t n = (int32_t)u32(); if (n <= 0 || at + n > d.size()) return ""; std::string s((const char*)&d[at], n - 1); at += n; return s; };
        u32(); const uint32_t ver = u32(); u32(); str(); const uint32_t flags = u32();
        const uint32_t nameCount = u32(), nameOff = u32(), expCount = u32(), expOff = u32(), impCount = u32(), impOff = u32();
        printf("version %08X flags %08X names %u@%X exports %u@%X imports %u@%X\n", ver, flags, nameCount, nameOff, expCount, expOff, impCount, impOff);
        std::vector<std::string> names; at = nameOff;
        for (uint32_t i = 0; i < nameCount && !bad; i++) { names.push_back(str()); u32(); u32(); }
        auto nm = [&](uint32_t i, uint32_t n) { std::string s = i < names.size() ? names[i] : "?"; if (n) s += "_" + std::to_string(n - 1); return s; };
        std::vector<std::string> impNames; at = impOff;
        for (uint32_t i = 0; i < impCount && !bad; i++) { u32(); u32(); u32(); u32(); u32(); const uint32_t a = u32(), b = u32(); impNames.push_back(nm(a, b)); }
        at = expOff; const char* want = argc >= 4 ? argv[3] : nullptr; int shown = 0;
        for (uint32_t i = 0; i < expCount && !bad; i++) {
            const int32_t cls = (int32_t)u32(); u32(); u32(); const uint32_t on = u32(), onum = u32(); u32(); u32(); u32();
            const uint32_t size = u32(), off = u32(); u32(); const uint32_t nets = u32(); at += nets * 4; at += 16; u32();
            const std::string c = cls < 0 && (size_t)(-cls - 1) < impNames.size() ? impNames[-cls - 1] : cls == 0 ? "Class" : "(export)";
            if (want && c != want) continue;
            if (shown++ < 40) printf("export %u %s '%s' size %u at %X\n", i, c.c_str(), nm(on, onum).c_str(), size, off);
        }
        printf("%s\n", bad ? "parse ran past the end" : "ok");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--find-shaders")) {                    // --find-shaders <file>: which recorded shaders (cache\shader_manifest.txt) occur in it, and where
        std::vector<uint8_t> data;
        if (!ue3_unpack_file(argv[2], data)) { std::ifstream f(argv[2], std::ios::binary); data.assign(std::istreambuf_iterator<char>(f), {}); }
        std::ifstream m(data_root() + "\\cache\\shader_manifest.txt");
        std::string line; int tried = 0, found = 0, partial = 0;
        while (std::getline(m, line)) {
            if (line.empty() || (line[0] != 'V' && line[0] != 'P')) continue;
            unsigned pg; size_t n; int used = 0;
            if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2) continue;
            std::vector<uint8_t> pat; const char* p = line.c_str() + 1 + used; unsigned v; int adv;
            while (sscanf(p, " %x%n", &v, &adv) == 1) { pat.push_back(v >> 24); pat.push_back(v >> 16); pat.push_back(v >> 8); pat.push_back(v); p += adv; }
            tried++;
            auto it = std::search(data.begin(), data.end(), pat.begin(), pat.end());
            if (it == data.end() && pat.size() >= 64) {                     // not verbatim: a 16-byte piece from the middle?
                const size_t mid = (pat.size() / 2) & ~3u;
                auto jt = std::search(data.begin(), data.end(), pat.begin() + mid, pat.begin() + mid + 16);
                if (jt != data.end()) { static int np = 0; partial++; if (np++ < 3) printf("%c %zu words: middle piece at %zu\n", line[0], n, (size_t)(jt - data.begin())); }
            }
            if (it != data.end()) {
                found++;
                const size_t at = it - data.begin();
                if (found <= 3) {
                    printf("%c %zu words at %zu; 64 bytes before:", line[0], n, at);
                    for (size_t k = at >= 64 ? at - 64 : 0; k < at; k++) printf("%s%02X", (k % 16) ? "" : "\n  ", data[k]);
                    printf("\n");
                }
            }
        }
        printf("%s: %zu bytes, %d of %d recorded shaders found, %d more by a middle piece\n", argv[2], data.size(), found, tried, partial);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--unpack")) {                          // --unpack <package> <out>: one cooked package, for checking the decompressor
        std::vector<uint8_t> out;
        const bool ok = ue3_unpack_file(argv[2], out);
        if (ok) if (FILE* f = fopen(argv[3], "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
        printf("%s: %s, %zu bytes\n", argv[2], ok ? "unpacked" : "failed", out.size());
        return ok ? 0 : 1;
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--build-shader-index")) {   // unpack the cooked packages into cache\shader_index.bin and exit
        const std::string cooked = data_root() + "\\game\\disc\\BCGame\\CookedXbox360", out = data_root() + "\\cache";
        int last = -1;
        const bool ok = shaderindex_build(cooked, out, [&](int done, int total) {
            const int pct = total ? done * 100 / total : 100;
            if (pct != last) { last = pct; fprintf(stderr, "\r[shaderindex] %d%% (%d of %d files)", pct, done, total); }
        });
        fprintf(stderr, "\n[shaderindex] %s\n", ok ? "done" : "failed");
        if (ok && shaderindex_load(out)) {                                   // coverage: recorded shaders (cache\shader_manifest.txt) the index holds
            std::ifstream m(out + "\\shader_manifest.txt"); std::string line; int tried = 0, found = 0, triedVs = 0, triedPs = 0, foundVs = 0, foundPs = 0, missLog = 0, patched = 0, patchedWords = 0;
            std::vector<uint32_t> code, got;
            while (std::getline(m, line)) {
                if (line.empty() || (line[0] != 'V' && line[0] != 'P')) continue;
                unsigned pg; size_t n; int used = 0; if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2) continue;
                code.clear(); const char* p = line.c_str() + 1 + used; unsigned v; int adv;
                while (sscanf(p, " %x%n", &v, &adv) == 1) { code.push_back(v); p += adv; }
                tried++;
                const bool vs = line[0] == 'V';
                (vs ? triedVs : triedPs)++;
                uint64_t ck; std::vector<uint32_t> base;
                if (shaderindex_find(shader_code_key(code.data(), code.size(), vs), got)) { found++; (vs ? foundVs : foundPs)++; }
                else if (shaderindex_closest(code, vs, ck, base)) {
                    found++; (vs ? foundVs : foundPs)++; patched++;
                    int d = 0; for (size_t k = 0; k < code.size(); k++) if (code[k] != base[k]) d++;
                    patchedWords += d;
                }
                else if (missLog++ < 3) fprintf(stderr, "[shaderindex] not found: %s of %zu words, paramGen %08X\n", vs ? "vs" : "ps", code.size(), pg);
            }
            fprintf(stderr, "[shaderindex] %zu shaders in the index; %d of %d recorded shaders found (vertex %d of %d, pixel %d of %d); %d of them patched, %d words in all\n",
                    shaderindex_size(), found, tried, foundVs, triedVs, foundPs, triedPs, patched, patchedWords);
        }
        return ok ? 0 : 1;
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--log")) g_logging = true;
    if (getenv("ASURA_LOG")) g_logging = true;
    if (g_logging) {
        std::string dir = data_root() + "\\logs"; CreateDirectoryA(dir.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t); char n[80];
        snprintf(n, sizeof n, "\\asura_%04d-%02d-%02d_%02d-%02d-%02d.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::string file = getenv("ASURA_LOGFILE") ? getenv("ASURA_LOGFILE") : dir + n;
        if (!freopen(file.c_str(), "w", stderr)) g_logging = false;
        setvbuf(stderr, nullptr, _IOFBF, 1 << 20);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { for (;;) { Sleep(500); fflush(stderr); } }, nullptr, 0, nullptr);
    }
    timeBeginPeriod(1);
    {   // opt out of Windows 11 power throttling: the game's threads are timed in milliseconds (bcr2 notes)
        struct { ULONG version, control, state; } pt{ 1, 0x1 | 0x4, 0 };
        SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4, &pt, sizeof pt);
    }
    AddVectoredExceptionHandler(1, CrashHandler);

    const std::string root = data_root();
    std::vector<char> image = read_file(root + "\\private\\image.bin"), xex = read_file(root + "\\private\\default_patched.xex");
    if (image.size() < PPC_IMAGE_SIZE || xex.size() < 0x1000 || memcmp(xex.data(), "XEX2", 4)) {
        fprintf(stderr, "missing or short private\\image.bin / private\\default_patched.xex under %s\n", root.c_str());
        return 1;
    }

    g_base = reserve_guest_memory();
    const uint64_t tableBytes = (PPC_CODE_SIZE * 2 + 0xFFFF) & ~0xFFFFull;
    commit((uint32_t)PPC_IMAGE_BASE, (size_t)(PPC_IMAGE_SIZE + tableBytes));
    memcpy(g_base + PPC_IMAGE_BASE, image.data(), (size_t)PPC_IMAGE_SIZE);
    {   uint64_t* t = (uint64_t*)(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
        for (uint64_t i = 0; i < PPC_CODE_SIZE / 4; i++) t[i] = (uint64_t)&UnresolvedIndirect; }
    size_t nf = 0;
    for (auto* m = PPCFuncMappings; m->host; ++m) {
        if (m->guest < PPC_CODE_BASE || m->guest >= PPC_CODE_BASE + PPC_CODE_SIZE) continue;
        *(PPCFunc**)(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(m->guest) - PPC_CODE_BASE) * 2) = m->host;
        nf++;
    }
    LOG("[main] %zu functions\n", nf);

    // imported variables get 256-byte blocks; functions point at their thunks
    commit(kKernelData, 0x40000);
    size_t ni; const ImportEntry* imps = host_import_table(ni);
    uint32_t var = kKernelData;
    for (size_t i = 0; i < ni; i++) {
        if (imps[i].thunk) wr32(imps[i].slot, imps[i].thunk);
        else { wr32(imps[i].slot, var); var += 0x100; }
    }

    // the XEX header (its offsets are relative to its start) and a loader entry for the executable
    const uint32_t peOff = _byteswap_ulong(*(uint32_t*)(xex.data() + 8));
    memcpy(g_base + kXexHeader, xex.data(), std::min<size_t>(peOff, 0x20000));
    g_xexHeaderGuest = kXexHeader;
    g_exeModule = kLdrEntry;
    wr32(kLdrEntry + 0x18, (uint32_t)PPC_IMAGE_BASE);
    wr32(kLdrEntry + 0x1C, (uint32_t)PPC_IMAGE_BASE);
    wr32(kLdrEntry + 0x20, (uint32_t)PPC_IMAGE_SIZE);
    wr32(kLdrEntry + 0x58, kXexHeader);
    {   uint32_t n = rd32(kXexHeader + 0x14);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = rd32(kXexHeader + 0x18 + i * 8), v = rd32(kXexHeader + 0x1C + i * 8);
            if (k == 0x00010100) wr32(kLdrEntry + 0x3C, v);
            if (k == 0x00040006) g_executionInfoGuest = kXexHeader + v;
        } }

    gpu_init_mmio();
    register_kernel_imports();
    register_file_imports();
    register_string_imports();
    register_xam_imports();
    register_gpu_imports();
    register_audio_imports();
    register_xma_imports();
    kernel_init_variables();
    monitor_start();
    LOG("[main] data root %s, %zu import records\n", root.c_str(), ni);

    HANDLE t = CreateThread(nullptr, 256ull << 20, [](LPVOID) -> DWORD {
        PPCContext ctx{}; guest_fp_init(ctx);
        GuestHostThread mt = make_host_guest_thread("main", 0x100000);
        bind_guest_thread(ctx, mt);
        uint32_t entry = rd32(kLdrEntry + 0x3C);
        LOG("[main] entry %08X\n", entry);
        call_guest(ctx, entry);
        fprintf(stderr, "[main] the entry point returned r3=%08X\n", ctx.r3.u32);
        return 0;
    }, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(t, INFINITE);
    fflush(stderr);
    return 0;
}
