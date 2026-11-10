// Asura's Wrath host: guest memory, the program image, the function table, imports, and the guest main thread.
//
// Guest memory map:
//   40000000-6FFFFFFF  virtual allocations (NtAllocateVirtualMemory, thread stacks and PCRs, pools)
//   7F000000           imported variables, the XEX header copy and the module's loader entry
//   7FC80000           GPU registers (memory-mapped)
//   82000000           the image, then the indirect-call table (8 bytes per code word)
//   A0000000-BFFFFFFF  physical memory, aliased at C0000000 and E0000000 like the console
#include "kernel.h"
#include <windows.h>
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
