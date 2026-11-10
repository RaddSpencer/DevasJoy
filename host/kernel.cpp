// xboxkrnl: import registry, memory, dispatcher objects and waits, threads, TLS, time, modules and the rest of the core.
// Ported from the BCR2 host (C:\dev\bcr2\host\imports.cpp), reorganised so every waitable object lives in guest memory.
#include "kernel.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

std::string guest_cstr(uint32_t a, size_t max) {
    if (!a) return "";
    std::string s;
    for (size_t i = 0; i < max && g_base[a + i]; i++) s += (char)g_base[a + i];
    return s;
}
std::string guest_ansi_string(uint32_t p) {
    if (!p) return "";
    uint16_t len = rd16(p);
    uint32_t buf = rd32(p + 4);
    return buf ? std::string((const char*)gp(buf), len) : std::string();
}

// ======================================================================== import registry
static const ImportEntry kImports[] = {
#include "import_table.inc"
};
const ImportEntry* host_import_table(size_t& count) { count = sizeof(kImports) / sizeof(kImports[0]); return kImports; }
static std::unordered_map<uint32_t, ImportFn>& registry() { static std::unordered_map<uint32_t, ImportFn> r; return r; }
static uint32_t ikey(int lib, int ord) { return (uint32_t(lib) << 16) | uint32_t(ord); }
static const char* import_name(int lib, int ord) {
    for (auto& e : kImports) if (e.lib == lib && e.ordinal == ord) return e.name;
    return "?";
}
void register_import(const char* name, ImportFn fn) {
    bool found = false;
    for (auto& e : kImports) if (!strcmp(e.name, name) && e.thunk) { registry()[ikey(e.lib, e.ordinal)] = fn; found = true; }
    if (!found) LOG("[imp] note: '%s' is not imported by this title\n", name);
}

// An import with no implementation: logged once each. ASURA_STRICT stops at the first one (the default is to return 0 and go on,
// which shows more of the boot at once but can mislead: check the log for what was skipped).
void ppc_import_dispatch(PPCContext& ctx, uint8_t* base, int lib, int ordinal) {
    auto it = registry().find(ikey(lib, ordinal));
    if (it != registry().end()) { it->second(ctx, base); return; }
    static std::mutex m; static std::unordered_map<uint32_t, int> seen;
    int n;
    { std::lock_guard<std::mutex> g(m); n = seen[ikey(lib, ordinal)]++; }
    if (n < 3)
        fprintf(stderr, "[imp] UNIMPLEMENTED %s (%s %d) lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X%s\n", import_name(lib, ordinal),
                lib == 0 ? "xboxkrnl" : "xam", ordinal, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, n == 2 ? " (no more reports)" : "");
    static const bool kStrict = getenv("ASURA_STRICT") != nullptr;
    if (kStrict) { fflush(stderr); TerminateProcess(GetCurrentProcess(), 2); }
    ctx.r3.u64 = 0;
}

uint32_t import_variable(const char* name) {
    for (auto& e : kImports) if (!e.thunk && !strcmp(e.name, name)) return rd32(e.slot);
    return 0;
}

// ======================================================================== memory
// Address ranges carry meaning on the console, and the game relies on it: UE3's allocator sends pointers below 0x40000000 and
// at 0xE0000000 and up to its large-block free path. So, as on the console:
//   4K-page virtual memory   0x00100000-0x3FFFFFFF
//   64K-page virtual memory  0x40000000-0x7EFFFFFF  (MEM_LARGE_PAGES; kernel stacks, PCRs and pools too)
//   physical memory          one 512 MB pool, returned at 0xA0000000 (64K pages), 0xC0000000 (16M pages) or 0xE0000000 (4K pages)
static void free_list_insert(std::map<uint32_t, uint32_t>& fl, uint32_t a, uint32_t sz) {
    auto it = fl.lower_bound(a);
    if (it != fl.begin()) { auto p = std::prev(it); if (p->first + p->second == a) { a = p->first; sz += p->second; fl.erase(p); } }
    it = fl.lower_bound(a);
    if (it != fl.end() && a + sz == it->first) { sz += it->second; fl.erase(it); }
    fl[a] = sz;
}
static bool free_list_take(std::map<uint32_t, uint32_t>& fl, uint32_t sz, uint32_t align, uint32_t& out) {
    for (auto it = fl.begin(); it != fl.end(); ++it) {
        const uint32_t s = it->first, e = s + it->second, a = (s + align - 1) & ~(align - 1);
        if ((uint64_t)a + sz > e) continue;
        fl.erase(it);
        if (a > s) fl[s] = a - s;
        if (a + sz < e) fl[a + sz] = e - (a + sz);
        out = a;
        return true;
    }
    return false;
}
static bool commit_range(uint32_t a, uint32_t size) {
    return VirtualAlloc(g_base + a, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
struct Arena {
    const char* name; uint32_t next, end, page;
    std::map<uint32_t, uint32_t> freeList, blocks;   // start -> size
    uint32_t place(uint32_t size, uint32_t align) {  // caller holds g_vmLock
        uint32_t rsz = (size + page - 1) & ~(page - 1), a = 0;
        align = std::max(align, page);
        if (!free_list_take(freeList, rsz, align, a)) {
            a = (next + align - 1) & ~(align - 1);
            if ((uint64_t)a + rsz > end) return 0;
            if (a > next) free_list_insert(freeList, next, a - next);
            next = a + rsz;
        }
        blocks[a] = rsz;
        return a;
    }
};
static std::mutex g_vmLock;
static Arena g_vm4k{ "4K", 0x00100000, 0x40000000, 0x1000 }, g_vm64k{ "64K", 0x40000000, 0x7F000000, 0x10000 };
static Arena& arena_of(uint32_t a) { return a < 0x40000000u ? g_vm4k : g_vm64k; }

uint32_t vm_alloc(uint32_t size) {
    std::lock_guard<std::mutex> g(g_vmLock);
    uint32_t a = g_vm64k.place(size, 0x10000);
    if (a && !commit_range(a, (size + 0xFFFF) & ~0xFFFFu)) return 0;
    return a;
}

// NtAllocateVirtualMemory(base*, size*, type, protect, debug): type MEM_COMMIT 0x1000, MEM_RESERVE 0x2000,
// MEM_LARGE_PAGES 0x20000000 (64K pages), MEM_16MB_PAGES 0x40000000.
static void NtAllocateVirtualMemory_(PPCContext& ctx, uint8_t*) {
    uint32_t baseP = ctx.r3.u32, sizeP = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = rd32(baseP), size = rd32(sizeP);
    if (!size) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    std::lock_guard<std::mutex> g(g_vmLock);
    const bool given = addr != 0;
    if (!addr) {
        Arena& ar = (type & 0x60000000) ? g_vm64k : g_vm4k;
        addr = ar.place(size, (type & 0x40000000) ? 0x1000000 : ar.page);
        if (!addr) { fprintf(stderr, "[mem] %s virtual arena full (%X wanted)\n", ar.name, size); ctx.r3.u64 = STATUS_NO_MEMORY; return; }
        size = ar.blocks[addr];
    } else {   // whole pages of the range's page size: committing in a 64K-page range commits whole 64K pages, and XAPI's heap relies on it
        const uint32_t page = arena_of(addr).page;
        uint32_t end = (uint32_t)(((uint64_t)addr + size + page - 1) & ~(uint64_t)(page - 1));
        addr &= ~(page - 1); size = end - addr;
    }
    // Only MEM_COMMIT commits: a reserve stays reserved, so the heap's own bookkeeping of committed pages (it asks NtQueryVirtualMemory) holds.
    if ((type & 0x1000) && !commit_range(addr, size)) {
        fprintf(stderr, "[mem] NtAllocateVirtualMemory: commit %08X+%X failed (%lu)\n", addr, size, GetLastError());
        ctx.r3.u64 = STATUS_NO_MEMORY; return;
    }
    wr32(baseP, addr); wr32(sizeP, size);
    LOG("[mem] NtAllocateVirtualMemory %08X+%X type=%08X%s lr=%08X\n", addr, size, type, given ? " (at given address)" : "", (uint32_t)ctx.lr);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtFreeVirtualMemory(base*, size*, type, debug): MEM_DECOMMIT 0x4000, MEM_RELEASE 0x8000
static void NtFreeVirtualMemory_(PPCContext& ctx, uint8_t*) {
    uint32_t baseP = ctx.r3.u32, sizeP = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = rd32(baseP), size = sizeP ? rd32(sizeP) : 0;
    std::lock_guard<std::mutex> g(g_vmLock);
    Arena& ar = arena_of(addr);
    LOG("[mem] NtFreeVirtualMemory %08X size=%X type=%X lr=%08X\n", addr, size, type, (uint32_t)ctx.lr);
    if (type & 0x8000) {
        auto it = ar.blocks.find(addr);
        if (it == ar.blocks.end()) { ctx.r3.u64 = STATUS_SUCCESS; return; }
        uint32_t sz = it->second;
        ar.blocks.erase(it);
        VirtualFree(g_base + addr, sz, MEM_DECOMMIT);
        free_list_insert(ar.freeList, addr, sz);
        if (sizeP) wr32(sizeP, sz);
    } else if (size) {
        VirtualFree(g_base + (addr & ~0xFFFu), size, MEM_DECOMMIT);
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION*): a region is the run of pages in one state from the queried page, and it
// never runs past the end of its allocation. Windows merges neighbouring committed blocks, and UE3's realloc grew a block in place
// over its neighbour; XAPI's heap needs the committed and reserved runs exactly. Addresses outside any allocation are MEM_FREE.
static void NtQueryVirtualMemory_(PPCContext& ctx, uint8_t*) {
    uint32_t addr = ctx.r3.u32, out = ctx.r4.u32;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_base + addr, &mbi, sizeof(mbi))) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    uint32_t rbase = (uint32_t)((uint8_t*)mbi.BaseAddress - g_base);
    uint64_t rsize = std::min<uint64_t>(mbi.RegionSize, 0x100000000ull - rbase);
    uint32_t ab = rbase & ~0xFFFFu;
    bool inAllocation = addr >= 0x80000000u;   // image and physical windows: as Windows reports them
    {   std::lock_guard<std::mutex> g(g_vmLock);
        Arena& ar = arena_of(addr);
        auto it = ar.blocks.upper_bound(addr);
        if (it != ar.blocks.begin()) {
            --it;
            if (addr < it->first + it->second) {
                inAllocation = true;
                ab = it->first;
                rsize = std::min<uint64_t>(rsize, it->first + it->second - rbase);
            }
        } }
    if (!inAllocation) mbi.State = MEM_FREE;
    LOG("[mem] NtQueryVirtualMemory %08X -> base %08X size %llX lr=%08X\n", addr, rbase, (unsigned long long)rsize, (uint32_t)ctx.lr);
    wr32(out + 0x00, rbase);
    wr32(out + 0x04, ab);
    wr32(out + 0x08, PAGE_READWRITE);
    wr32(out + 0x0C, (uint32_t)std::min<uint64_t>(rsize, 0xFFFFFFFFull));
    wr32(out + 0x10, mbi.State);
    wr32(out + 0x14, mbi.State == MEM_COMMIT ? PAGE_READWRITE : 0);
    wr32(out + 0x18, MEM_PRIVATE);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// physical memory: offsets 0..0x1FFFFFFF of the one section main.cpp maps at A0, C0 and E0000000
static std::mutex g_physLock;
static uint32_t g_physBump = 0x00010000;                    // the first 64K stays free: the GPU's write-back words live low
static std::unordered_map<uint32_t, uint32_t> g_physSizes;  // physical offset -> size
static std::map<uint32_t, uint32_t> g_physFree;
static uint32_t phys_place(uint32_t size, uint32_t align) {
    if (!align || (align & (align - 1))) align = 0x1000;
    align = std::max<uint32_t>(align, 0x1000);
    uint32_t sz = (size + 0xFFF) & ~0xFFFu, off = 0;
    std::lock_guard<std::mutex> g(g_physLock);
    if (free_list_take(g_physFree, sz, align, off)) memset(g_base + 0xA0000000u + off, 0, sz);
    else {
        off = (g_physBump + align - 1) & ~(align - 1);
        if ((uint64_t)off + sz > 0x20000000ull) return 0xFFFFFFFFu;
        if (off > g_physBump) free_list_insert(g_physFree, g_physBump, off - g_physBump);
        g_physBump = off + sz;
    }
    g_physSizes[off] = sz;
    return off;
}
uint32_t phys_alloc(uint32_t size, uint32_t align) {   // kernel side: the 64K-page window
    uint32_t off = phys_place(size, align);
    return off == 0xFFFFFFFFu ? 0 : 0xA0000000u | off;
}
// The 0xE0000000 window (4K pages) is 0x1000 ahead of the others on the console: virtual E0000000+x is physical x+0x1000, and the
// GPU is handed physical addresses (D3D's fence at E0012000 is written by the GPU to physical 0x13000). The host maps every window
// at the same section offset (views must be 64K aligned), so each E0 block gets one spare page at its end and this table marks the
// physical pages that belong to E0 blocks: the GPU side translates those 0x1000 lower.
static uint8_t g_e0Page[0x20000];
bool phys_page_is_e0(uint32_t p) { return g_e0Page[(p & 0x1FFFFFFFu) >> 12] != 0; }
uint32_t gpu_phys_to_host(uint32_t p) {
    p &= 0x1FFFFFFFu;
    return g_e0Page[p >> 12] ? 0xE0000000u + p - 0x1000u : 0xA0000000u + p;
}
static void mark_e0(uint32_t off, uint32_t size, uint8_t v) {
    for (uint32_t pg = (off + 0x1000) >> 12, end = (off + 0x1000 + size) >> 12; pg < end && pg < 0x20000; pg++) g_e0Page[pg] = v;
}
// MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment): protect & 0x20000000 64K pages, & 0x80000000 16M pages, else 4K
static void MmAllocatePhysicalMemoryEx_(PPCContext& ctx, uint8_t*) {
    uint32_t prot = ctx.r5.u32, size = ctx.r4.u32, align = ctx.r8.u32;
    uint32_t window = (prot & 0x80000000u) ? 0xC0000000u : (prot & 0x20000000u) ? 0xA0000000u : 0xE0000000u;
    uint32_t page = window == 0xC0000000u ? 0x1000000u : window == 0xA0000000u ? 0x10000u : 0x1000u;
    uint32_t sz = (size + page - 1) & ~(page - 1);
    uint32_t off = phys_place(sz + (window == 0xE0000000u ? 0x1000 : 0), std::max(align, page));
    uint32_t a = off == 0xFFFFFFFFu ? 0 : window | off;
    if (a && window == 0xE0000000u) mark_e0(off, sz, 1);
    // An A0/C0 block owns its pages outright: clear any E0 marks left there by an earlier block (a stale mark shifted every GPU
    // read of the texture pool by 0x1000, so 64x32 textures and many mip chains came out scrambled or black).
    if (a && window != 0xE0000000u) for (uint32_t pg = off >> 12, end = (off + sz) >> 12; pg < end && pg < 0x20000; pg++) g_e0Page[pg] = 0;
    if (!a) fprintf(stderr, "[mem] physical memory full (%X wanted)\n", size);
    LOG("[mem] MmAllocatePhysicalMemoryEx %X align=%X prot=%X -> %08X lr=%08X\n", size, align, prot, a, (uint32_t)ctx.lr);
    ctx.r3.u64 = a;
}
static void MmFreePhysicalMemory_(PPCContext& ctx, uint8_t*) {
    uint32_t off = ctx.r4.u32 & 0x1FFFFFFFu;
    std::lock_guard<std::mutex> g(g_physLock);
    auto it = g_physSizes.find(off);
    if (it == g_physSizes.end()) return;
    if (ctx.r4.u32 >= 0xE0000000u) mark_e0(off, it->second - 0x1000, 0);
    free_list_insert(g_physFree, off, it->second);
    g_physSizes.erase(it);
}
static void MmQueryAllocationSize_(PPCContext& ctx, uint8_t*) {
    uint32_t addr = ctx.r3.u32, sz = 0;
    if (addr >= 0xA0000000u) { std::lock_guard<std::mutex> g(g_physLock); auto it = g_physSizes.find(addr & 0x1FFFFFFFu); if (it != g_physSizes.end()) sz = it->second; }
    else { std::lock_guard<std::mutex> g(g_vmLock); Arena& ar = arena_of(addr); auto it = ar.blocks.find(addr); if (it != ar.blocks.end()) sz = it->second; }
    ctx.r3.u64 = sz;
}
static void MmGetPhysicalAddress_(PPCContext& ctx, uint8_t*) {
    uint32_t a = ctx.r3.u32;
    ctx.r3.u64 = (a & 0x1FFFFFFF) + (a >= 0xE0000000u ? 0x1000 : 0);
}
static void MmMapIoSpace_(PPCContext& ctx, uint8_t*) { LOG("[mem] MmMapIoSpace %08X+%X\n", ctx.r3.u32, ctx.r4.u32); }   // r3 unchanged
static void MmQueryAddressProtect_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = PAGE_READWRITE; }
static void MmQueryStatistics_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r3.u32;
    if (!p) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    uint32_t physUsed; { std::lock_guard<std::mutex> g(g_physLock); physUsed = g_physBump / 0x1000; }
    wr32(p + 0x00, 0x68);
    wr32(p + 0x04, 0x20000);                  // total pages: 512 MB
    wr32(p + 0x08, 0x1000);                   // kernel pages
    wr32(p + 0x0C, 0x20000 - 0x1000 - physUsed);   // available
    wr32(p + 0x10, 0x30000000);               // title virtual bytes
    wr32(p + 0x14, 0x01000000);
    wr32(p + 0x18, physUsed);
    for (uint32_t o = 0x1C; o < 0x68; o += 4) wr32(p + o, 0x100);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// pool: small kernel allocations, carved from 64K arena blocks
static std::mutex g_poolLock;
static std::unordered_map<uint32_t, uint32_t> g_poolSizes;
static std::multimap<uint32_t, uint32_t> g_poolFree;    // size -> address
static uint32_t g_poolCur = 0, g_poolLeft = 0;
static uint32_t pool_alloc(uint32_t size) {
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    std::lock_guard<std::mutex> g(g_poolLock);
    auto it = g_poolFree.lower_bound(size);
    if (it != g_poolFree.end() && it->first < size * 2) { uint32_t a = it->second; g_poolFree.erase(it); memset(g_base + a, 0, g_poolSizes[a]); return a; }
    if (size > 0x8000) { uint32_t a = vm_alloc(size); if (a) g_poolSizes[a] = (size + 0xFFFF) & ~0xFFFFu; return a; }
    if (g_poolLeft < size) { g_poolCur = vm_alloc(0x100000); g_poolLeft = g_poolCur ? 0x100000 : 0; }
    if (!g_poolCur) return 0;
    uint32_t a = g_poolCur; g_poolCur += size; g_poolLeft -= size;
    g_poolSizes[a] = size;
    return a;
}
static void ExAllocatePoolTypeWithTag_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = pool_alloc(ctx.r3.u32); }
static void ExAllocatePool_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = pool_alloc(ctx.r3.u32); }
static void ExFreePool_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_poolLock);
    auto it = g_poolSizes.find(ctx.r3.u32);
    if (it != g_poolSizes.end()) g_poolFree.emplace(it->second, it->first);
}

// ======================================================================== dispatcher objects and waits
static std::mutex g_obLock;                 // guards every signal state and the handle table
static std::condition_variable g_obCv;
static std::unordered_map<uint32_t, uint32_t> g_handles;      // handle -> object
static std::unordered_map<uint32_t, uint32_t> g_objHandle;    // object -> first handle
static std::atomic<uint32_t> g_nextHandle{0xF8000004};
// mutant ownership: object -> owning KTHREAD
static std::unordered_map<uint32_t, uint32_t> g_mutantOwner;

uint32_t ob_create(uint8_t type, int32_t state, uint32_t size) {
    uint32_t o = pool_alloc(size);
    g_base[o] = type; g_base[o + 2] = (uint8_t)(size / 4);
    wr32(o + 4, (uint32_t)state);
    wr32(o + 8, o + 8); wr32(o + 12, o + 8);    // empty wait list
    return o;
}
void ob_signal_changed() { g_obCv.notify_all(); }
uint32_t handle_create(uint32_t obj) {
    uint32_t h = g_nextHandle.fetch_add(4);
    std::lock_guard<std::mutex> g(g_obLock);
    g_handles[h] = obj;
    if (!g_objHandle.count(obj)) g_objHandle[obj] = h;
    return h;
}
uint32_t handle_object(uint32_t h) {
    if (h == 0xFFFFFFFE) return thread_current_object();          // NtCurrentThread
    std::lock_guard<std::mutex> g(g_obLock);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? 0 : it->second;
}
void handle_close(uint32_t h) { std::lock_guard<std::mutex> g(g_obLock); g_handles.erase(h); }

// caller holds g_obLock
static bool ob_ready(uint32_t o, uint32_t self) {
    uint8_t type = g_base[o];
    int32_t st = (int32_t)rd32(o + 4);
    if (type == 2) { auto it = g_mutantOwner.find(o); return st > 0 || (it != g_mutantOwner.end() && it->second == self); }
    return st > 0 || (type != 5 && st != 0);
}
static void ob_consume(uint32_t o, uint32_t self) {
    uint8_t type = g_base[o];
    int32_t st = (int32_t)rd32(o + 4);
    switch (type) {
      case 1: case 9: wr32(o + 4, 0); break;            // synchronization event / timer: auto-reset
      case 5: wr32(o + 4, (uint32_t)(st - 1)); break;   // semaphore
      case 2: wr32(o + 4, (uint32_t)(st - 1)); g_mutantOwner[o] = self; break;
      default: break;                                   // notification objects stay signalled
    }
}
int64_t guest_timeout(uint32_t ptr, bool& has) {
    has = ptr != 0;
    return has ? (int64_t)rd64(ptr) : 0;
}
// ---- thread monitor (with --log): every 10 s, what each guest thread is waiting on, or where it is when it isn't waiting ----
struct MonSlot {
    std::atomic<PPCContext*> ctx{nullptr}; DWORD tid = 0; HANDLE h = nullptr; uint32_t kthread = 0; ULONGLONG lastCpu = 0;
    std::atomic<uint32_t> waitObj{0}, waitLr{0}; std::atomic<uint64_t> waitSince{0}; const char* waitKind = "";
};
static MonSlot g_mon[256];
static std::atomic<int> g_monN{0};
static thread_local MonSlot* t_mon = nullptr;
static void mon_register(PPCContext* ctx, uint32_t kthread) {
    if (t_mon) { t_mon->ctx = ctx; t_mon->kthread = kthread; return; }
    int i = g_monN.fetch_add(1);
    if (i >= 256) return;
    MonSlot& s = g_mon[i];
    s.tid = GetCurrentThreadId(); s.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.tid); s.kthread = kthread; s.ctx = ctx;
    t_mon = &s;
}
struct MonWait {
    MonWait(const char* kind, uint32_t obj) {
        if (!t_mon) return;
        t_mon->waitKind = kind; t_mon->waitObj = obj; t_mon->waitLr = g_ctx ? (uint32_t)g_ctx->lr : 0; t_mon->waitSince = GetTickCount64();
    }
    ~MonWait() { if (t_mon) t_mon->waitSince = 0; }
};
static void monitor_thread() {
    for (;;) {
        Sleep(10000);
        fprintf(stderr, "[monitor] ---- %d guest threads ----\n", g_monN.load());
        if (const char* w = getenv("ASURA_WATCH")) {   // ASURA_WATCH=addr,addr,... (hex guest addresses): their words
            fprintf(stderr, "[monitor] watch:");
            for (const char* p = w; *p;) {
                char* e; uint32_t a = (uint32_t)strtoul(p, &e, 16);
                if (e == p) break;
                if (!IsBadReadPtr(g_base + a, 4)) fprintf(stderr, " [%08X]=%08X", a, rd32(a));
                p = (*e == ',') ? e + 1 : e;
            }
            fprintf(stderr, "\n");
        }
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < g_monN.load() && i < 256; i++) {
            MonSlot& s = g_mon[i];
            FILETIME c, e, k, u; ULONGLONG cpu = 0;
            if (s.h && GetThreadTimes(s.h, &c, &e, &k, &u)) cpu = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
            DWORD code = 0; if (s.h && GetExitCodeThread(s.h, &code) && code != STILL_ACTIVE) continue;
            uint64_t since = s.waitSince.load();
            PPCContext* x = s.ctx.load();
            uint32_t tid = s.kthread ? rd32(s.kthread + 0x14C) : 0;
            if (since) {
                uint32_t o = s.waitObj.load();
                fprintf(stderr, "[monitor] thread %u: %s wait on %08X (type %u state %d) from lr=%08X for %llu ms, cpu +%llu ms\n", tid, s.waitKind, o,
                        (o && !IsBadReadPtr(g_base + o, 8)) ? g_base[o] : 0, (o && !IsBadReadPtr(g_base + o, 8)) ? (int)rd32(o + 4) : 0, s.waitLr.load(), (unsigned long long)(now - since), (unsigned long long)(cpu - s.lastCpu));
            } else {
                fprintf(stderr, "[monitor] thread %u: running, lr=%08X r1=%08X r3=%08X, cpu +%llu ms, stack:", tid, x ? (uint32_t)x->lr : 0, x ? x->r1.u32 : 0, x ? x->r3.u32 : 0,
                        (unsigned long long)(cpu - s.lastCpu));
                // r1 is a snapshot of a running thread and can be stale: check every read (an unmapped stack page took the process down)
                for (uint32_t sp = x ? x->r1.u32 : 0, k = 0; k < 12 && sp >= 0x40000000u && sp < 0x7F000000u; k++) {
                    if (IsBadReadPtr(g_base + sp, 4)) break;
                    uint32_t next = rd32(sp);
                    if (next <= sp || next - sp > 0x100000 || IsBadReadPtr(g_base + next - 8, 4)) break;
                    fprintf(stderr, " %08X", rd32(next - 8));
                    sp = next;
                }
                fprintf(stderr, "\n");
            }
            s.lastCpu = cpu;
        }
        fflush(stderr);
    }
}
void monitor_start() { if (g_logging) std::thread(monitor_thread).detach(); }

// ---- APCs: I/O completion routines queue on the thread that started the I/O and run at its next alertable wait or sleep ----
struct Apc { uint32_t routine, a1, a2, a3; };
static thread_local std::deque<Apc> t_apcs;
void apc_queue(uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3) { t_apcs.push_back({ routine, a1, a2, a3 }); }
static bool apc_deliver() {
    if (t_apcs.empty() || !g_ctx) return false;
    PPCContext* self = g_ctx;
    while (!t_apcs.empty()) {
        Apc a = t_apcs.front(); t_apcs.pop_front();
        PPCContext c = *self;
        c.r1.u64 = (self->r1.u32 - 0x200) & ~0xFull;   // below the caller's frame
        c.r3.u64 = a.a1; c.r4.u64 = a.a2; c.r5.u64 = a.a3;
        call_guest(c, a.routine);
        g_ctx = self;
    }
    return true;
}
constexpr uint32_t kStatusUserApc = 0xC0;   // STATUS_USER_APC

uint32_t ke_wait(const uint32_t* objs, uint32_t count, bool waitAll, int64_t t100, bool hasTimeout, bool alertable) {
    if (alertable && apc_deliver()) return kStatusUserApc;
    MonWait mw(count > 1 ? "multi" : "single", count ? objs[0] : 0);
    using clk = std::chrono::steady_clock;
    clk::time_point deadline = clk::time_point::max();
    if (hasTimeout) {
        if (t100 < 0) deadline = clk::now() + std::chrono::microseconds(-t100 / 10);
        else {   // absolute system time in 100 ns units
            FILETIME ft; GetSystemTimeAsFileTime(&ft);
            int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
            deadline = clk::now() + std::chrono::microseconds(t100 > now ? (t100 - now) / 10 : 0);
        }
    }
    const uint32_t self = thread_current_object();
    std::unique_lock<std::mutex> lk(g_obLock);
    for (;;) {
        if (waitAll) {
            bool all = true;
            for (uint32_t i = 0; i < count && all; i++) all = ob_ready(objs[i], self);
            if (all) { for (uint32_t i = 0; i < count; i++) ob_consume(objs[i], self); return STATUS_SUCCESS; }
        } else {
            for (uint32_t i = 0; i < count; i++) if (ob_ready(objs[i], self)) { ob_consume(objs[i], self); return i; }
        }
        auto now = clk::now();
        if (now >= deadline) return STATUS_TIMEOUT;
        // a short slice even for long waits: guest code may set a signal state directly without a kernel call
        auto slice = std::min<clk::duration>(deadline - now, std::chrono::milliseconds(5));
        g_obCv.wait_for(lk, slice);
    }
}

// Ke* on object pointers
static void KeSetEvent_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_obLock);
    uint32_t o = ctx.r3.u32, prev = rd32(o + 4);
    wr32(o + 4, 1);
    g_obCv.notify_all();
    ctx.r3.u64 = prev;
}
static void KeResetEvent_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_obLock);
    uint32_t o = ctx.r3.u32, prev = rd32(o + 4);
    wr32(o + 4, 0);
    ctx.r3.u64 = prev;
}
// KeWaitForSingleObject(obj, reason, mode, alertable, timeout*)
static void KeWaitForSingleObject_(PPCContext& ctx, uint8_t*) {
    uint32_t o = ctx.r3.u32; bool has; int64_t t = guest_timeout(ctx.r7.u32, has);
    ctx.r3.u64 = ke_wait(&o, 1, true, t, has, ctx.r6.u32 != 0);
}
// KeWaitForMultipleObjects(count, objs**, type, reason, mode, alertable, timeout*, blocks*)
static void KeWaitForMultipleObjects_(PPCContext& ctx, uint8_t*) {
    uint32_t n = ctx.r3.u32, arr = ctx.r4.u32;
    if (n == 0 || n > 64) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    uint32_t objs[64]; for (uint32_t i = 0; i < n; i++) objs[i] = rd32(arr + i * 4);
    bool has; int64_t t = guest_timeout(ctx.r9.u32, has);
    ctx.r3.u64 = ke_wait(objs, n, ctx.r5.u32 == 0, t, has, ctx.r8.u32 != 0);
}

// Nt* on handles
static uint32_t obj_of(uint32_t h, PPCContext& ctx) {
    uint32_t o = handle_object(h);
    if (!o) LOG("[ob] unknown handle %08X lr=%08X\n", h, (uint32_t)ctx.lr);
    return o;
}
// NtCreateEvent(handle*, attributes*, type (0 notification, 1 synchronization), initial)
static void NtCreateEvent_(PPCContext& ctx, uint8_t*) {
    uint32_t o = ob_create(ctx.r5.u32 ? 1 : 0, ctx.r6.u32 ? 1 : 0, 0x10);
    wr32(ctx.r3.u32, handle_create(o));
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void ev_set(PPCContext& ctx, int32_t v, bool pulse) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t prev;
    {   std::lock_guard<std::mutex> g(g_obLock);
        prev = rd32(o + 4); wr32(o + 4, (uint32_t)v);
        if (v) g_obCv.notify_all(); }
    if (pulse) {   // let current waiters through, then reset
        std::this_thread::yield();
        std::lock_guard<std::mutex> g(g_obLock); wr32(o + 4, 0);
    }
    if (ctx.r4.u32) wr32(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtSetEvent_(PPCContext& ctx, uint8_t*) { ev_set(ctx, 1, false); }
static void NtPulseEvent_(PPCContext& ctx, uint8_t*) { ev_set(ctx, 1, true); }
static void NtClearEvent_(PPCContext& ctx, uint8_t*) { ctx.r4.u64 = 0; ev_set(ctx, 0, false); }
void event_set_by_handle(uint32_t h) {
    uint32_t o = handle_object(h);
    if (!o) return;
    std::lock_guard<std::mutex> g(g_obLock); wr32(o + 4, 1); g_obCv.notify_all();
}
// NtCreateSemaphore(handle*, attributes*, initial, maximum)
static void NtCreateSemaphore_(PPCContext& ctx, uint8_t*) {
    uint32_t o = ob_create(5, (int32_t)ctx.r5.u32, 0x14);
    wr32(o + 16, ctx.r6.u32);
    wr32(ctx.r3.u32, handle_create(o));
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtReleaseSemaphore(handle, count, previous*)
static void NtReleaseSemaphore_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(g_obLock);
    int32_t st = (int32_t)rd32(o + 4), lim = (int32_t)rd32(o + 16), add = (int32_t)ctx.r4.u32;
    if (ctx.r5.u32) wr32(ctx.r5.u32, (uint32_t)st);
    if (lim > 0 && st + add > lim) { ctx.r3.u64 = STATUS_SEMAPHORE_LIMIT_EXCEEDED; return; }
    wr32(o + 4, (uint32_t)(st + add));
    g_obCv.notify_all();
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtCreateMutant(handle*, attributes*, initial_owner)
static void NtCreateMutant_(PPCContext& ctx, uint8_t*) {
    uint32_t o = ob_create(2, ctx.r5.u32 ? 0 : 1, 0x20);
    if (ctx.r5.u32) { std::lock_guard<std::mutex> g(g_obLock); g_mutantOwner[o] = thread_current_object(); }
    wr32(ctx.r3.u32, handle_create(o));
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtReleaseMutant(handle, previous*)
static void NtReleaseMutant_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(g_obLock);
    int32_t st = (int32_t)rd32(o + 4);
    if (ctx.r4.u32) wr32(ctx.r4.u32, (uint32_t)st);
    auto it = g_mutantOwner.find(o);
    if (it == g_mutantOwner.end() || it->second != thread_current_object()) { ctx.r3.u64 = STATUS_MUTANT_NOT_OWNED; return; }
    wr32(o + 4, (uint32_t)(st + 1));
    if (st + 1 > 0) g_mutantOwner.erase(it);
    g_obCv.notify_all();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// timers: a host thread sets the timer's state when due
struct TimerState { std::atomic<uint64_t> gen{0}; };
static std::unordered_map<uint32_t, std::shared_ptr<TimerState>> g_timers;
// NtCreateTimer(handle*, attributes*, type (0 notification, 1 synchronization))
static void NtCreateTimer_(PPCContext& ctx, uint8_t*) {
    uint32_t o = ob_create(ctx.r5.u32 ? 9 : 8, 0, 0x28);
    { std::lock_guard<std::mutex> g(g_obLock); g_timers[o] = std::make_shared<TimerState>(); }
    wr32(ctx.r3.u32, handle_create(o));
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtSetTimerEx(handle, due*, apc, apc_mode, apc_ctx, resume, period_ms, previous*)
static void NtSetTimerEx_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int64_t due = (int64_t)rd64(ctx.r4.u32);
    uint32_t period = ctx.r9.u32, apc = ctx.r5.u32;
    if (apc) LOG("[timer] APC routine %08X ignored\n", apc);
    std::shared_ptr<TimerState> ts;
    { std::lock_guard<std::mutex> g(g_obLock); ts = g_timers[o]; if (!ts) ts = g_timers[o] = std::make_shared<TimerState>(); wr32(o + 4, 0); }
    uint64_t gen = ++ts->gen;
    int64_t firstUs = due < 0 ? -due / 10 : 0;
    std::thread([o, ts, gen, firstUs, period] {
        std::this_thread::sleep_for(std::chrono::microseconds(firstUs));
        for (;;) {
            if (ts->gen.load() != gen) return;
            { std::lock_guard<std::mutex> g(g_obLock); wr32(o + 4, 1); g_obCv.notify_all(); }
            if (!period) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(period));
        }
    }).detach();
    if (ctx.r10.u32) wr32(ctx.r10.u32, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtCancelTimer_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    std::lock_guard<std::mutex> g(g_obLock);
    auto it = g_timers.find(o);
    if (it != g_timers.end()) it->second->gen++;
    if (ctx.r4.u32) wr32(ctx.r4.u32, o ? rd32(o + 4) : 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NtWaitForSingleObjectEx(handle, mode, alertable, timeout*)
static void NtWaitForSingleObjectEx_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    bool has; int64_t t = guest_timeout(ctx.r6.u32, has);
    ctx.r3.u64 = ke_wait(&o, 1, true, t, has, ctx.r5.u32 != 0);
}
// NtWaitForMultipleObjectsEx(count, handles*, type, mode, alertable, timeout*)
static void NtWaitForMultipleObjectsEx_(PPCContext& ctx, uint8_t*) {
    uint32_t n = ctx.r3.u32, arr = ctx.r4.u32;
    if (n == 0 || n > 64) { ctx.r3.u64 = STATUS_INVALID_PARAMETER; return; }
    uint32_t objs[64];
    for (uint32_t i = 0; i < n; i++) if (!(objs[i] = obj_of(rd32(arr + i * 4), ctx))) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    bool has; int64_t t = guest_timeout(ctx.r8.u32, has);
    ctx.r3.u64 = ke_wait(objs, n, ctx.r5.u32 == 0, t, has, ctx.r7.u32 != 0);
}
// ObReferenceObjectByHandle(handle, type, object*)
static void ObReferenceObjectByHandle_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) wr32(ctx.r5.u32, o);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtDuplicateObject(handle, new*, options)
static void NtDuplicateObject_(PPCContext& ctx, uint8_t*) {
    uint32_t o = obj_of(ctx.r3.u32, ctx);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r4.u32) wr32(ctx.r4.u32, handle_create(o));
    if (ctx.r5.u32 & 1) handle_close(ctx.r3.u32);   // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = STATUS_SUCCESS;
}
void file_close(uint32_t handle);   // kfile.cpp
static void NtClose_(PPCContext& ctx, uint8_t*) { file_close(ctx.r3.u32); handle_close(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }

// critical sections: one host recursive mutex per guest critical section
static std::mutex g_csMapLock;
static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_cs;
static std::recursive_mutex& cs_mutex(uint32_t cs) {
    std::lock_guard<std::mutex> g(g_csMapLock);
    auto& p = g_cs[cs];
    if (!p) p = std::make_unique<std::recursive_mutex>();
    return *p;
}
static void RtlInitializeCriticalSection_(PPCContext& ctx, uint8_t*) { cs_mutex(ctx.r3.u32); ctx.r3.u64 = 0; }
static void RtlEnterCriticalSection_(PPCContext& ctx, uint8_t*) {
    auto& m = cs_mutex(ctx.r3.u32);
    if (m.try_lock()) return;
    MonWait mw("critical section", ctx.r3.u32);
    m.lock();
}
static void RtlLeaveCriticalSection_(PPCContext& ctx, uint8_t*) { cs_mutex(ctx.r3.u32).unlock(); }
static void RtlTryEnterCriticalSection_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = cs_mutex(ctx.r3.u32).try_lock() ? 1 : 0; }

// spin locks: a guest word, 0 = free
static void spin_acquire(uint32_t a) {
    volatile LONG* p = (volatile LONG*)(g_base + a);
    const LONG want = (LONG)_byteswap_ulong(thread_current_object() | 1);
    for (int n = 0; InterlockedCompareExchange(p, want, 0) != 0; n++) { if (n > 1000) SwitchToThread(); else YieldProcessor(); }
}
static void KfAcquireSpinLock_(PPCContext& ctx, uint8_t*) { spin_acquire(ctx.r3.u32); ctx.r3.u64 = 0; }
static void KfReleaseSpinLock_(PPCContext& ctx, uint8_t*) { InterlockedExchange((volatile LONG*)(g_base + ctx.r3.u32), 0); }
static void KeAcquireSpinLockAtRaisedIrql_(PPCContext& ctx, uint8_t*) { spin_acquire(ctx.r3.u32); }

// interlocked singly linked lists: header {u32 next; u16 depth; u16 sequence}
static std::mutex g_slistLock;
static void InterlockedPopEntrySList_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t h = ctx.r3.u32, first = rd32(h);
    if (first) { wr32(h, rd32(first)); uint16_t d = rd16(h + 4); wr16(h + 4, d ? d - 1 : 0); wr16(h + 6, rd16(h + 6) + 1); }
    ctx.r3.u64 = first;
}
static void InterlockedPushEntrySList_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t h = ctx.r3.u32, e = ctx.r4.u32, first = rd32(h);
    wr32(e, first); wr32(h, e); wr16(h + 4, rd16(h + 4) + 1); wr16(h + 6, rd16(h + 6) + 1);
    ctx.r3.u64 = first;
}
static void InterlockedFlushSList_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_slistLock);
    uint32_t h = ctx.r3.u32, first = rd32(h);
    wr32(h, 0); wr16(h + 4, 0);
    ctx.r3.u64 = first;
}

// ======================================================================== threads
// PCR (r13): +0x00 TLS pointer, +0x70 stack base (high), +0x74 stack limit, +0x100 current KTHREAD, +0x10C processor number.
// KTHREAD: dispatcher header (type 6, signalled at exit), +0x14C thread id.
struct ThreadInfo { uint32_t kthread, pcr, stack, stackSize, id; HANDLE host; std::string name; };
static std::mutex g_thrLock;
static std::unordered_map<uint32_t, std::shared_ptr<ThreadInfo>> g_threads;   // KTHREAD -> info
static std::atomic<uint32_t> g_nextTid{1};
static thread_local uint32_t t_kthread = 0;
static thread_local uint32_t t_tls[64];

uint32_t thread_current_object() { return t_kthread; }

static std::shared_ptr<ThreadInfo> thread_alloc(uint32_t stackSize) {
    auto ti = std::make_shared<ThreadInfo>();
    stackSize = (std::max<uint32_t>(stackSize, 0x10000) + 0xFFFF) & ~0xFFFFu;
    ti->pcr = vm_alloc(0x10000);
    ti->stack = vm_alloc(stackSize);
    ti->stackSize = stackSize;
    ti->kthread = ti->pcr + 0x1000;
    ti->id = g_nextTid++;
    g_base[ti->kthread] = 6; wr32(ti->kthread + 4, 0);
    wr32(ti->kthread + 8, ti->kthread + 8); wr32(ti->kthread + 12, ti->kthread + 8);
    wr32(ti->kthread + 0x14C, ti->id);
    wr32(ti->pcr + 0x00, ti->pcr + 0x800);           // TLS block (unused: the title has no TLS data)
    wr32(ti->pcr + 0x70, ti->stack + stackSize);
    wr32(ti->pcr + 0x74, ti->stack);
    wr32(ti->pcr + 0x100, ti->kthread);
    g_base[ti->pcr + 0x10C] = (uint8_t)(ti->id % 6);
    std::lock_guard<std::mutex> g(g_thrLock);
    g_threads[ti->kthread] = ti;
    return ti;
}
static void bind(PPCContext& ctx, const ThreadInfo& ti) {
    ctx.r1.u64 = ti.stack + ti.stackSize - 0x100;
    ctx.r13.u64 = ti.pcr;
    t_kthread = ti.kthread;
    mon_register(&ctx, ti.kthread);
}
GuestHostThread make_host_guest_thread(const char* name, uint32_t stackSize) {
    auto ti = thread_alloc(stackSize);
    ti->name = name;
    LOG("[thread] host-driven guest thread '%s' id=%u\n", name, ti->id);
    return { ti->pcr, ti->kthread, ti->stack + ti->stackSize };
}
void bind_guest_thread(PPCContext& ctx, const GuestHostThread& t) {
    ctx.r1.u64 = t.stackTop - 0x100;
    ctx.r13.u64 = t.pcr;
    t_kthread = t.kthread;
    mon_register(&ctx, t.kthread);
}
void call_guest(PPCContext& ctx, uint32_t fn) {
    uint8_t* base = g_base;
    PPCContext* prev = g_ctx; g_ctx = &ctx;
    ctx.lr = 0;
    PPC_CALL_INDIRECT_FUNC(fn);
    g_ctx = prev;
}

void guest_fp_init(PPCContext& ctx);   // main.cpp
struct ThreadStart { std::shared_ptr<ThreadInfo> ti; uint32_t entry, a0, a1; };
static DWORD WINAPI GuestThreadProc(LPVOID p) {
    std::unique_ptr<ThreadStart> ts((ThreadStart*)p);
    PPCContext ctx{}; guest_fp_init(ctx);
    g_ctx = &ctx;
    bind(ctx, *ts->ti);
    ctx.r3.u64 = ts->a0; ctx.r4.u64 = ts->a1;
    uint8_t* base = g_base;
    ctx.lr = 0;
    PPC_CALL_INDIRECT_FUNC(ts->entry);
    {   std::lock_guard<std::mutex> g(g_obLock); wr32(ts->ti->kthread + 4, 1); g_obCv.notify_all(); }
    LOG("[thread] %u returned %08X\n", ts->ti->id, ctx.r3.u32);
    return ctx.r3.u32;
}
// ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start, start_ctx, flags)
static void ExCreateThread_(PPCContext& ctx, uint8_t*) {
    uint32_t hP = ctx.r3.u32, idP = ctx.r5.u32, startup = ctx.r6.u32, start = ctx.r7.u32, sctx = ctx.r8.u32, flags = ctx.r9.u32;
    auto ti = thread_alloc(ctx.r4.u32);
    if (!ti->pcr || !ti->stack) { ctx.r3.u64 = STATUS_NO_MEMORY; return; }
    auto* ts = new ThreadStart{ ti, startup ? startup : start, startup ? start : sctx, startup ? sctx : 0 };
    ti->host = CreateThread(nullptr, 64ull << 20, GuestThreadProc, ts, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    uint32_t h = handle_create(ti->kthread);
    if (hP) wr32(hP, h);
    if (idP) wr32(idP, ti->id);
    LOG("[thread] ExCreateThread id=%u entry=%08X arg=%08X stack=%X flags=%X lr=%08X\n", ti->id, ts->entry, ts->a0, ti->stackSize, flags, (uint32_t)ctx.lr);
    if (!(flags & 1)) ResumeThread(ti->host);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static std::shared_ptr<ThreadInfo> thread_by_object(uint32_t o) {
    std::lock_guard<std::mutex> g(g_thrLock);
    auto it = g_threads.find(o);
    return it == g_threads.end() ? nullptr : it->second;
}
static void NtResumeThread_(PPCContext& ctx, uint8_t*) {
    auto ti = thread_by_object(handle_object(ctx.r3.u32));
    if (!ti || !ti->host) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    DWORD prev = ResumeThread(ti->host);
    if (ctx.r4.u32) wr32(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtSuspendThread_(PPCContext& ctx, uint8_t*) {
    auto ti = thread_by_object(handle_object(ctx.r3.u32));
    if (!ti || !ti->host) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    DWORD prev = SuspendThread(ti->host);
    if (ctx.r4.u32) wr32(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void KeResumeThread_(PPCContext& ctx, uint8_t*) {
    auto ti = thread_by_object(ctx.r3.u32);
    ctx.r3.u64 = (ti && ti->host) ? ResumeThread(ti->host) : 0;
}
static void KeSetAffinityThread_(PPCContext& ctx, uint8_t*) { if (ctx.r5.u32) wr32(ctx.r5.u32, 0x3F); ctx.r3.u64 = 0x3F; }
static void KeSetBasePriorityThread_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
static void KeQueryBasePriorityThread_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
// KeDelayExecutionThread(mode, alertable, interval*): INT64_MIN is "forever" (SleepEx(INFINITE)); an alertable sleep returns
// STATUS_USER_APC after running queued APCs, which is how XAPI's ReadFileEx completion reaches the game
static void KeDelayExecutionThread_(PPCContext& ctx, uint8_t*) {
    const bool alertable = ctx.r4.u32 != 0;
    if (alertable && apc_deliver()) { ctx.r3.u64 = kStatusUserApc; return; }
    int64_t v = ctx.r5.u32 ? (int64_t)rd64(ctx.r5.u32) : 0;
    MonWait mw("sleep", 0);
    if (v == INT64_MIN) {
        fprintf(stderr, "[thread] %u sleeps forever (alertable=%d) lr=%08X\n", thread_current_object() ? rd32(thread_current_object() + 0x14C) : 0, (int)alertable, (uint32_t)ctx.lr);
        for (;;) Sleep(INFINITE);
    }
    if (v < 0) {
        int64_t us = -v / 10;
        if (us == 0) SwitchToThread();
        else std::this_thread::sleep_for(std::chrono::microseconds(us));
    } else SwitchToThread();
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtYieldExecution_(PPCContext& ctx, uint8_t*) { SwitchToThread(); ctx.r3.u64 = 0; }
static void ExTerminateThread_(PPCContext& ctx, uint8_t*) {
    uint32_t self = thread_current_object();
    { std::lock_guard<std::mutex> g(g_obLock); if (self) wr32(self + 4, 1); g_obCv.notify_all(); }
    LOG("[thread] ExTerminateThread code=%u\n", ctx.r3.u32);
    ExitThread(ctx.r3.u32);
}

// TLS
static std::atomic<uint32_t> g_tlsNext{0};
static void KeTlsAlloc_(PPCContext& ctx, uint8_t*) { uint32_t i = g_tlsNext++; ctx.r3.u64 = i < 64 ? i : 0xFFFFFFFFu; }
static void KeTlsFree_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void KeTlsSetValue_(PPCContext& ctx, uint8_t*) { uint32_t i = ctx.r3.u32; if (i < 64) t_tls[i] = ctx.r4.u32; ctx.r3.u64 = i < 64; }
static void KeTlsGetValue_(PPCContext& ctx, uint8_t*) { uint32_t i = ctx.r3.u32; ctx.r3.u64 = i < 64 ? t_tls[i] : 0; }

// DPCs: run on a guest thread of their own
static void KeInitializeDpc_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r3.u32;
    memset(g_base + p, 0, 0x1C);
    g_base[p] = 19;
    wr32(p + 0x0C, ctx.r4.u32); wr32(p + 0x10, ctx.r5.u32);
}
static void KeInsertQueueDpc_(PPCContext& ctx, uint8_t*) {
    uint32_t dpc = ctx.r3.u32, a1 = ctx.r4.u32, a2 = ctx.r5.u32;
    static GuestHostThread t = make_host_guest_thread("dpc");
    static std::mutex m;
    std::thread([dpc, a1, a2] {
        std::lock_guard<std::mutex> g(m);
        PPCContext c{}; guest_fp_init(c); bind_guest_thread(c, t);
        c.r3.u64 = dpc; c.r4.u64 = rd32(dpc + 0x10); c.r5.u64 = a1; c.r6.u64 = a2;
        call_guest(c, rd32(dpc + 0x0C));
    }).detach();
    ctx.r3.u64 = 1;
}

// ======================================================================== time
static uint64_t filetime_now() { FILETIME ft; GetSystemTimeAsFileTime(&ft); return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
static void KeQuerySystemTime_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr64(ctx.r3.u32, filetime_now()); }
// mftb is recompiled to __rdtsc(), so the game's timebase runs at the host TSC rate: report that (see the BCR2 notes, 74x fast at 50 MHz).
uint64_t tsc_hz() {
    static const uint64_t hz = [] {
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        uint64_t t0 = __rdtsc();
        do { QueryPerformanceCounter(&b); } while ((uint64_t)(b.QuadPart - a.QuadPart) * 1000 < (uint64_t)f.QuadPart * 50);
        uint64_t t1 = __rdtsc();
        return (t1 - t0) * (uint64_t)f.QuadPart / (uint64_t)(b.QuadPart - a.QuadPart);
    }();
    return hz;
}
static void KeQueryPerformanceFrequency_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = tsc_hz(); }
// TIME_FIELDS: u16 year, month, day, hour, minute, second, milliseconds, weekday
static void RtlTimeToTimeFields_(PPCContext& ctx, uint8_t*) {
    uint64_t t = rd64(ctx.r3.u32); FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) }; SYSTEMTIME st; FileTimeToSystemTime(&ft, &st);
    uint32_t o = ctx.r4.u32;
    wr16(o, st.wYear); wr16(o + 2, st.wMonth); wr16(o + 4, st.wDay); wr16(o + 6, st.wHour);
    wr16(o + 8, st.wMinute); wr16(o + 10, st.wSecond); wr16(o + 12, st.wMilliseconds); wr16(o + 14, st.wDayOfWeek);
}
static void RtlTimeFieldsToTime_(PPCContext& ctx, uint8_t*) {
    uint32_t i = ctx.r3.u32;
    SYSTEMTIME st{ rd16(i), rd16(i + 2), rd16(i + 14), rd16(i + 4), rd16(i + 6), rd16(i + 8), rd16(i + 10), rd16(i + 12) };
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) { ctx.r3.u64 = 0; return; }
    wr64(ctx.r4.u32, ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 1;
}

// ======================================================================== modules and the XEX header
// main.cpp copies the XEX header into guest memory and builds a loader entry for the executable.
uint32_t g_xexHeaderGuest = 0, g_exeModule = 0;
// RtlImageXexHeaderField(xex_header*, key) -> value (inline keys) or pointer to the field
static uint32_t xex_header_field(uint32_t hdr, uint32_t key) {
    if (!hdr) return 0;
    uint32_t n = rd32(hdr + 0x14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = rd32(hdr + 0x18 + i * 8), v = rd32(hdr + 0x1C + i * 8);
        if (k != key) continue;
        uint8_t low = key & 0xFF;
        return (low == 0 || low == 1) ? v : hdr + v;
    }
    return 0;
}
static void RtlImageXexHeaderField_(PPCContext& ctx, uint8_t*) {
    uint32_t r = xex_header_field(ctx.r3.u32, ctx.r4.u32);
    LOG("[xex] header field %08X -> %08X\n", ctx.r4.u32, r);
    ctx.r3.u64 = r;
}
// XexGetModuleHandle(name, handle*)
static void XexGetModuleHandle_(PPCContext& ctx, uint8_t*) {
    std::string n = guest_cstr(ctx.r3.u32);
    uint32_t h = (!ctx.r3.u32 || _stricmp(n.c_str(), "default.xex") == 0) ? g_exeModule : 0;
    LOG("[xex] XexGetModuleHandle '%s' -> %08X\n", n.c_str(), h);
    if (ctx.r4.u32) wr32(ctx.r4.u32, h);
    ctx.r3.u64 = h ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}
// XexGetModuleSection(module, name, data*, size*): the XEX resource table (header key 0x2FF)
static void XexGetModuleSection_(PPCContext& ctx, uint8_t*) {
    std::string want = guest_cstr(ctx.r4.u32);
    uint32_t res = xex_header_field(g_xexHeaderGuest, 0x000002FF);
    if (res) {
        uint32_t n = (rd32(res) - 4) / 16;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = res + 4 + i * 16;
            std::string nm((const char*)gp(e), strnlen((const char*)gp(e), 8));
            if (_stricmp(nm.c_str(), want.c_str())) continue;
            if (ctx.r5.u32) wr32(ctx.r5.u32, rd32(e + 8));
            if (ctx.r6.u32) wr32(ctx.r6.u32, rd32(e + 12));
            LOG("[xex] section '%s' -> %08X+%X\n", want.c_str(), rd32(e + 8), rd32(e + 12));
            ctx.r3.u64 = STATUS_SUCCESS; return;
        }
    }
    LOG("[xex] section '%s' not found\n", want.c_str());
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}
static void XexGetProcedureAddress_(PPCContext& ctx, uint8_t*) {
    LOG("[xex] XexGetProcedureAddress module=%08X ordinal=%u\n", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r5.u32) wr32(ctx.r5.u32, 0);
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}
static void XexLoadImage_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[xex] XexLoadImage '%s' flags=%X lr=%08X: other modules are not supported\n", guest_cstr(ctx.r3.u32).c_str(), ctx.r4.u32, (uint32_t)ctx.lr);
    if (ctx.r6.u32) wr32(ctx.r6.u32, 0);
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}
static void XexLoadImageHeaders_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[xex] XexLoadImageHeaders '%s'\n", guest_ansi_string(ctx.r3.u32).c_str());
    ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
}

// ======================================================================== misc system
static void ExGetXConfigSetting_(PPCContext& ctx, uint8_t*) {
    uint16_t cat = (uint16_t)ctx.r3.u32, setting = (uint16_t)ctx.r4.u32;
    uint32_t buf = ctx.r5.u32, bufSize = ctx.r6.u32, req = ctx.r7.u32;
    uint32_t value = 0, size = 4;
    if (cat == 3 && setting == 9) value = 1;                  // language: English
    else if (cat == 3 && setting == 10) value = 0;            // video flags
    else if (cat == 3 && setting == 0x11) value = 0x00010001; // AV region
    else if (cat == 2 && setting == 2) { value = 0x40; size = 2; }   // game region
    else LOG("[xconfig] category %u setting %u -> 0\n", cat, setting);
    if (req) wr16(req, (uint16_t)size);
    if (bufSize < size) { ctx.r3.u64 = STATUS_BUFFER_TOO_SMALL; return; }
    if (buf) { if (size == 2) wr16(buf, (uint16_t)value); else wr32(buf, value); }
    ctx.r3.u64 = STATUS_SUCCESS;
}
// RtlRaiseException(EXCEPTION_RECORD*): 0x406D1388 names a thread; anything else is reported
static void RtlRaiseException_(PPCContext& ctx, uint8_t*) {
    uint32_t rec = ctx.r3.u32, code = rd32(rec);
    if (code == 0x406D1388) {
        uint32_t info = rec + 20;   // {type 0x1000, name*, thread id, flags}
        std::string name = guest_cstr(rd32(info + 4), 64);
        LOG("[thread] name of %08X: %s\n", rd32(info + 8), name.c_str());
        return;
    }
    fprintf(stderr, "[raise] RtlRaiseException code=%08X flags=%08X addr=%08X lr=%08X\n", code, rd32(rec + 4), rd32(rec + 12), (uint32_t)ctx.lr);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
}
static void KeBugCheckEx_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[bugcheck] code=%08X %08X %08X %08X %08X lr=%08X\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr);
    fflush(stderr); TerminateProcess(GetCurrentProcess(), 5);
}
static void HalReturnToFirmware_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[exit] HalReturnToFirmware(%u)\n", ctx.r3.u32); fflush(stderr); TerminateProcess(GetCurrentProcess(), 0);
}
static void DbgBreakPoint_(PPCContext& ctx, uint8_t*) { LOG("[dbg] DbgBreakPoint lr=%08X\n", (uint32_t)ctx.lr); }
// XeCryptSha(in1, len1, in2, len2, in3, len3, out, outlen)
static void XeCryptSha_(PPCContext& ctx, uint8_t*) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    uint32_t parts[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (auto& p : parts) if (p[0] && p[1]) BCryptHashData(h, g_base + p[0], p[1], 0);
    uint8_t d[20]; BCryptFinishHash(h, d, 20, 0);
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0);
    uint32_t out = ctx.r9.u32, n = std::min<uint32_t>(ctx.r10.u32, 20);
    if (out) memcpy(g_base + out, d, n);
}
static void ReturnZero_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
static void ReturnOne_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void NoOp_(PPCContext&, uint8_t*) {}
static void Unsuccessful_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = STATUS_UNSUCCESSFUL; }
static void KeGetCurrentProcessType_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }   // title
static void ObIsTitleObject_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }

// ======================================================================== variables
static void time_bundle_thread(uint32_t bundle) {
    // KeTimeStampBundle {u64 interrupt_time; u64 system_time; u32 tick_count; u32 pad}: XAPI's GetTickCount reads tick_count
    const uint64_t t0 = GetTickCount64();
    for (;;) {
        uint64_t now = GetTickCount64() - t0;
        wr64(bundle + 0, now * 10000);
        wr64(bundle + 8, filetime_now());
        wr32(bundle + 16, (uint32_t)now);
        Sleep(1);
    }
}
void kernel_init_variables() {
    if (uint32_t v = import_variable("KeTimeStampBundle")) std::thread(time_bundle_thread, v).detach();
    if (uint32_t v = import_variable("XboxHardwareInfo")) { wr32(v, 0x20); g_base[v + 4] = 6; }   // flags, 6 hardware threads
    if (uint32_t v = import_variable("XboxKrnlVersion")) { wr16(v, 2); wr16(v + 2, 0xFFFF); wr16(v + 4, 0xFFFF); wr16(v + 6, 0x8000); }
    if (uint32_t v = import_variable("XexExecutableModuleHandle")) wr32(v, g_exeModule);
    if (uint32_t v = import_variable("ExLoadedCommandLine")) {
        const char* cl = getenv("ASURA_CMDLINE") ? getenv("ASURA_CMDLINE") : "\"default.xex\"";
        strncpy((char*)gp(v), cl, 0xFF);
    }
    if (uint32_t v = import_variable("VdGpuClockInMHz")) wr32(v, 500);
    if (uint32_t v = import_variable("ExEventObjectType")) wr32(v, 0xEEEE0001);
    if (uint32_t v = import_variable("ExThreadObjectType")) wr32(v, 0xEEEE0006);
}

void register_kernel_imports() {
    register_import("NtAllocateVirtualMemory", NtAllocateVirtualMemory_);
    register_import("NtFreeVirtualMemory", NtFreeVirtualMemory_);
    register_import("NtQueryVirtualMemory", NtQueryVirtualMemory_);
    register_import("MmAllocatePhysicalMemoryEx", MmAllocatePhysicalMemoryEx_);
    register_import("MmFreePhysicalMemory", MmFreePhysicalMemory_);
    register_import("MmQueryAllocationSize", MmQueryAllocationSize_);
    register_import("MmGetPhysicalAddress", MmGetPhysicalAddress_);
    register_import("MmMapIoSpace", MmMapIoSpace_);
    register_import("MmQueryAddressProtect", MmQueryAddressProtect_);
    register_import("MmSetAddressProtect", NoOp_);
    register_import("MmQueryStatistics", MmQueryStatistics_);
    register_import("ExAllocatePool", ExAllocatePool_);
    register_import("ExAllocatePoolTypeWithTag", ExAllocatePoolTypeWithTag_);
    register_import("ExFreePool", ExFreePool_);

    register_import("KeSetEvent", KeSetEvent_);
    register_import("KeResetEvent", KeResetEvent_);
    register_import("KeWaitForSingleObject", KeWaitForSingleObject_);
    register_import("KeWaitForMultipleObjects", KeWaitForMultipleObjects_);
    register_import("NtCreateEvent", NtCreateEvent_);
    register_import("NtSetEvent", NtSetEvent_);
    register_import("NtPulseEvent", NtPulseEvent_);
    register_import("NtClearEvent", NtClearEvent_);
    register_import("NtCreateSemaphore", NtCreateSemaphore_);
    register_import("NtReleaseSemaphore", NtReleaseSemaphore_);
    register_import("NtCreateMutant", NtCreateMutant_);
    register_import("NtReleaseMutant", NtReleaseMutant_);
    register_import("NtCreateTimer", NtCreateTimer_);
    register_import("NtSetTimerEx", NtSetTimerEx_);
    register_import("NtCancelTimer", NtCancelTimer_);
    register_import("NtWaitForSingleObjectEx", NtWaitForSingleObjectEx_);
    register_import("NtWaitForMultipleObjectsEx", NtWaitForMultipleObjectsEx_);
    register_import("ObReferenceObjectByHandle", ObReferenceObjectByHandle_);
    register_import("ObReferenceObject", NoOp_);
    register_import("ObDereferenceObject", NoOp_);
    register_import("ObIsTitleObject", ObIsTitleObject_);
    register_import("NtDuplicateObject", NtDuplicateObject_);
    register_import("NtClose", NtClose_);

    register_import("RtlInitializeCriticalSection", RtlInitializeCriticalSection_);
    register_import("RtlInitializeCriticalSectionAndSpinCount", RtlInitializeCriticalSection_);
    register_import("RtlEnterCriticalSection", RtlEnterCriticalSection_);
    register_import("RtlLeaveCriticalSection", RtlLeaveCriticalSection_);
    register_import("RtlTryEnterCriticalSection", RtlTryEnterCriticalSection_);
    register_import("KfAcquireSpinLock", KfAcquireSpinLock_);
    register_import("KfReleaseSpinLock", KfReleaseSpinLock_);
    register_import("KeAcquireSpinLockAtRaisedIrql", KeAcquireSpinLockAtRaisedIrql_);
    register_import("KeReleaseSpinLockFromRaisedIrql", KfReleaseSpinLock_);
    register_import("InterlockedPopEntrySList", InterlockedPopEntrySList_);
    register_import("InterlockedPushEntrySList", InterlockedPushEntrySList_);
    register_import("InterlockedFlushSList", InterlockedFlushSList_);
    register_import("KeEnterCriticalRegion", NoOp_);
    register_import("KeLeaveCriticalRegion", NoOp_);
    register_import("KeLockL2", NoOp_);
    register_import("KeUnlockL2", NoOp_);

    register_import("ExCreateThread", ExCreateThread_);
    register_import("NtResumeThread", NtResumeThread_);
    register_import("NtSuspendThread", NtSuspendThread_);
    register_import("KeResumeThread", KeResumeThread_);
    register_import("KeSetAffinityThread", KeSetAffinityThread_);
    register_import("KeSetBasePriorityThread", KeSetBasePriorityThread_);
    register_import("KeQueryBasePriorityThread", KeQueryBasePriorityThread_);
    register_import("KeDelayExecutionThread", KeDelayExecutionThread_);
    register_import("NtYieldExecution", NtYieldExecution_);
    register_import("ExTerminateThread", ExTerminateThread_);
    register_import("KeTlsAlloc", KeTlsAlloc_);
    register_import("KeTlsFree", KeTlsFree_);
    register_import("KeTlsSetValue", KeTlsSetValue_);
    register_import("KeTlsGetValue", KeTlsGetValue_);
    register_import("KeInitializeDpc", KeInitializeDpc_);
    register_import("KeInsertQueueDpc", KeInsertQueueDpc_);
    register_import("KiApcNormalRoutineNop", NoOp_);

    register_import("KeQuerySystemTime", KeQuerySystemTime_);
    register_import("KeQueryPerformanceFrequency", KeQueryPerformanceFrequency_);
    register_import("RtlTimeToTimeFields", RtlTimeToTimeFields_);
    register_import("RtlTimeFieldsToTime", RtlTimeFieldsToTime_);

    register_import("RtlImageXexHeaderField", RtlImageXexHeaderField_);
    register_import("XexGetModuleHandle", XexGetModuleHandle_);
    register_import("XexGetModuleSection", XexGetModuleSection_);
    register_import("XexGetProcedureAddress", XexGetProcedureAddress_);
    register_import("XexLoadImage", XexLoadImage_);
    register_import("XexLoadImageHeaders", XexLoadImageHeaders_);
    register_import("XexUnloadImage", ReturnZero_);
    register_import("XexCheckExecutablePrivilege", ReturnZero_);

    register_import("ExGetXConfigSetting", ExGetXConfigSetting_);
    register_import("ExRegisterTitleTerminateNotification", NoOp_);
    register_import("KeGetCurrentProcessType", KeGetCurrentProcessType_);
    register_import("KeSetCurrentProcessType", NoOp_);
    register_import("RtlRaiseException", RtlRaiseException_);
    register_import("KeBugCheck", KeBugCheckEx_);
    register_import("KeBugCheckEx", KeBugCheckEx_);
    register_import("HalReturnToFirmware", HalReturnToFirmware_);
    register_import("DbgBreakPoint", DbgBreakPoint_);
    register_import("XeCryptSha", XeCryptSha_);
    register_import("XeKeysConsolePrivateKeySign", ReturnZero_);
    register_import("XeKeysConsoleSignatureVerification", ReturnZero_);
    register_import("FscSetCacheElementCount", ReturnZero_);
    register_import("IoCheckShareAccess", ReturnZero_);
    register_import("IoSetShareAccess", NoOp_);
    register_import("IoRemoveShareAccess", NoOp_);
    register_import("IoCreateDevice", Unsuccessful_);
    register_import("IoDeleteDevice", NoOp_);
    register_import("IoCompleteRequest", NoOp_);
    register_import("IoInvalidDeviceRequest", Unsuccessful_);
    register_import("IoDismountVolume", ReturnZero_);
    register_import("IoDismountVolumeByFileHandle", ReturnZero_);
    register_import("StfsCreateDevice", Unsuccessful_);
    register_import("StfsControlDevice", Unsuccessful_);
    (void)ReturnOne_;
}
