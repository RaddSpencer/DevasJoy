// Command processor: video-kernel calls (Vd*), the command ring and the PM4 interpreter. Register writes and every type-3 packet
// that passes its predicate go to the renderer (xenos.cpp, from BCR2: state, shaders, draws, resolves, present); this file does the
// side effects the CPU depends on (fence and memory writes, scratch registers, waits, interrupts). ASURA_NOGPU keeps it a null GPU.
// Predicated packets are skipped unless bin select & mask is set: running them anyway was the BCR2 crash root cause (bcr2 notes).
#include "kernel.h"
#include "gpu.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

void guest_fp_init(PPCContext& ctx);
static constexpr uint32_t kMmio = 0x7FC80000;   // GPU registers, reg n at kMmio + 4n
static inline uint32_t reg_addr(uint32_t r) { return kMmio + r * 4; }
static uint32_t g_regs[0x10000];

struct Gpu {
    uint32_t intrCb = 0, intrCtx = 0;
    uint32_t ringBase = 0, ringLog2 = 0, wbAddr = 0;
    uint64_t binMask = ~0ull, binSelect = ~0ull;
    std::atomic<uint64_t> swaps{0}, interrupts{0}, packets{0};
} g_gpu;
static std::mutex g_intrLock;
static GuestHostThread g_gpuThread, g_vsyncThread;
static std::atomic<uint32_t> g_opCount[128];

uint32_t gpu_phys_to_host(uint32_t p);   // kernel.cpp: physical address -> guest address, E0 pages shifted
static uint32_t phys(uint32_t a) { return gpu_phys_to_host(a); }
// endian modes in the low 2 bits of a PM4 address: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32
static void write_endian(uint32_t addr, uint32_t v) {
    uint32_t mode = addr & 3, a = phys(addr & ~3u);
    switch (mode) {
      case 0: *(uint32_t*)(g_base + a) = v; break;   // guest memory is big-endian: no swap means the host-order bytes
      case 1: *(uint32_t*)(g_base + a) = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF); break;
      case 2: wr32(a, v); break;
      case 3: *(uint32_t*)(g_base + a) = (v << 16) | (v >> 16); break;
    }
}
static uint32_t read_endian(uint32_t addr) {
    uint32_t mode = addr & 3, a = phys(addr & ~3u), raw = *(uint32_t*)(g_base + a);
    switch (mode) {
      case 1: return ((raw & 0x00FF00FF) << 8) | ((raw >> 8) & 0x00FF00FF);
      case 2: return _byteswap_ulong(raw);
      case 3: return (raw << 16) | (raw >> 16);
      default: return raw;
    }
}

static void call_interrupt(uint32_t source, uint32_t cpu, GuestHostThread& t) {
    if (!g_gpu.intrCb) return;
    std::lock_guard<std::mutex> g(g_intrLock);
    PPCContext c{}; guest_fp_init(c);
    bind_guest_thread(c, t);
    g_base[t.pcr + 0x10C] = (uint8_t)cpu;
    c.r3.u64 = source; c.r4.u64 = g_gpu.intrCtx;
    call_guest(c, g_gpu.intrCb);
}

static const bool kRender = gpu_enabled();
static void reg_write(uint32_t r, uint32_t v) {
    r &= 0xFFFF;
    g_regs[r] = v;
    if (kRender) gpu_reg(r, v);
    wr32(reg_addr(r), v);
    if (r >= 0x0578 && r <= 0x057F) {   // SCRATCH_REG0-7: mirrored to SCRATCH_ADDR when enabled in SCRATCH_UMSK
        uint32_t n = r - 0x0578;
        if ((g_regs[0x01DC] >> n) & 1) wr32(phys(g_regs[0x01DD] + n * 4), v);
    }
}
static bool cond(uint32_t func, uint32_t value, uint32_t ref) {
    switch (func & 7) {
      case 0: return false; case 1: return value < ref; case 2: return value <= ref; case 3: return value == ref;
      case 4: return value != ref; case 5: return value >= ref; case 6: return value > ref; default: return true;
    }
}

static void execute(const uint32_t* p, uint32_t n, int depth);
static uint32_t g_predSkipped = 0;                 // draws skipped by bin predication since the last swap (ASURA_PREDLOG)
static uint64_t g_predLastSel = 0, g_predLastMask = 0;
static void exec_indirect(uint32_t addr, uint32_t dwords, int depth) {
    if (depth > 4 || dwords > 0x400000) return;
    std::vector<uint32_t> v(dwords);
    for (uint32_t k = 0; k < dwords; k++) v[k] = rd32(phys(addr) + k * 4);
    execute(v.data(), dwords, depth + 1);
}
static void type3(uint32_t op, const uint32_t* d, uint32_t cnt, int depth) {
    if (kRender) gpu_packet(g_base, op, d, cnt);    // renderer first: state, constants, shaders, draws, resolves, swaps
    switch (op) {
      case 0x3F: case 0x37:                       // INDIRECT_BUFFER(_PFD)
        exec_indirect(d[0] & ~3u, d[1] & 0xFFFFF, depth); break;
      case 0x3D: {                                // MEM_WRITE: address, then data
        uint32_t a = d[0];
        for (uint32_t k = 1; k < cnt; k++) write_endian((a & ~3u) + (k - 1) * 4 | (a & 3), d[k]);
        break; }
      case 0x58: {                                // EVENT_WRITE_SHD: initiator, address, value (bit 31 of the initiator: write the swap count)
        uint32_t v = (d[0] >> 31) ? (uint32_t)g_gpu.swaps.load() : d[2];
        write_endian(d[1], v); break; }
      case 0x5A: {                                // EVENT_WRITE_EXT: screen extents of the last draw. Reported as everything, as Xenia and
        if (cnt < 2) break;                       // BCR2 do: games use them to pick tiles, and zeros cull nearly every draw. {minX, maxX, minY, maxY, minZ, maxZ}
        uint32_t a = phys(d[1] & ~3u);
        static const uint16_t ext[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
        for (int k = 0; k < 6; k++) wr16(a + k * 2, ext[k]);
        break; }
      case 0x45: {                                // COND_WRITE: func, poll address/reg, ref, mask, write address/reg, value
        uint32_t func = d[0], value = (func & 0x10) ? read_endian(d[1]) : g_regs[d[1] & 0xFFFF];
        if (cond(func, value & d[3], d[2])) {
            if (func & 0x100) write_endian(d[4], d[5]); else reg_write(d[4], d[5]);
        }
        break; }
      case 0x3C: {                                // WAIT_REG_MEM: wait until a memory word meets the condition (register polls are not emulated)
        if (!(d[0] & 0x10)) break;
        const uint32_t fn = d[0], ref = d[2], mask = d[3];
        auto t0 = std::chrono::steady_clock::now();
        for (int w = 0; !cond(fn, read_endian(d[1]) & mask, ref); w++) {
            if (w < 2000) std::this_thread::yield(); else std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) {
                static int n = 0;
                if (n++ < 10) fprintf(stderr, "[gpu] WAIT_REG_MEM timed out at %08X (value %08X, ref %08X, function %u)\n", d[1], read_endian(d[1]), ref, fn & 7);
                break;
            }
        }
        break; }
      case 0x60: g_gpu.binMask = (g_gpu.binMask & 0xFFFFFFFF00000000ull) | d[0]; break;
      case 0x61: g_gpu.binMask = (g_gpu.binMask & 0xFFFFFFFFull) | ((uint64_t)d[0] << 32); break;
      case 0x62: g_gpu.binSelect = (g_gpu.binSelect & 0xFFFFFFFF00000000ull) | d[0]; break;
      case 0x63: g_gpu.binSelect = (g_gpu.binSelect & 0xFFFFFFFFull) | ((uint64_t)d[0] << 32); break;
      case 0x54: {                                // INTERRUPT: one callback per CPU in the mask, finished before the ring goes on
        g_gpu.interrupts++;
        for (uint32_t cpu = 0; cpu < 6; cpu++) if (d[0] & (1u << cpu)) call_interrupt(1, cpu, g_gpuThread);
        break; }
      case 0x64: {                                // XE_SWAP (written by VdSwap): 'SWAP', front buffer
        uint64_t n = ++g_gpu.swaps;
        if (n <= 3 || n % 600 == 0) LOG("[gpu] swap #%llu front=%08X\n", (unsigned long long)n, cnt > 1 ? d[1] : 0);
        if (g_predSkipped) {
            fprintf(stderr, "[pred] swap %llu: %u draws predicated out (last select %016llX mask %016llX)\n", (unsigned long long)n, g_predSkipped,
                    (unsigned long long)g_predLastSel, (unsigned long long)g_predLastMask);
            g_predSkipped = 0;
        }
        break; }
      default: break;
    }
}
static void execute(const uint32_t* p, uint32_t n, int depth) {
    for (uint32_t i = 0; i < n;) {
        uint32_t h = p[i], type = h >> 30;
        if (type == 0) {
            uint32_t r = h & 0x7FFF, c = ((h >> 16) & 0x3FFF) + 1;
            bool one = (h >> 15) & 1;
            for (uint32_t k = 0; k < c && i + 1 + k < n; k++) reg_write(one ? r : r + k, p[i + 1 + k]);
            i += 1 + c;
        } else if (type == 1) {
            if (i + 2 < n) { reg_write(h & 0x7FF, p[i + 1]); reg_write((h >> 11) & 0x7FF, p[i + 2]); }
            i += 3;
        } else if (type == 2) {
            i += 1;
        } else {
            uint32_t c = ((h >> 16) & 0x3FFF) + 1, op = (h >> 8) & 0x7F;
            if (i + 1 + c > n) { LOG("[gpu] truncated packet op=%02X\n", op); return; }
            g_gpu.packets++;
            if (g_logging && (op == 0x58 || op == 0x45 || op == 0x3C || op == 0x54) && g_opCount[op] < 12) {
                fprintf(stderr, "[pm4] op %02X%s n=%u:", op, (h & 1) ? " (predicated)" : "", c);
                for (uint32_t k = 0; k < c && k < 8; k++) fprintf(stderr, " %08X", p[i + 1 + k]);
                fprintf(stderr, "\n");
            }
            g_opCount[op]++;
            if (!(h & 1) || (g_gpu.binSelect & g_gpu.binMask)) type3(op, p + i + 1, c, depth);
            else if (op == 0x22 || op == 0x36) {                          // a draw predicated out: counted per frame (ASURA_PREDLOG)
                static const bool kLog = getenv("ASURA_PREDLOG") != nullptr;
                if (kLog) { g_predSkipped++; g_predLastSel = g_gpu.binSelect; g_predLastMask = g_gpu.binMask; }
            }
            i += 1 + c;
        }
    }
}

static void gpu_thread() {
    g_gpuThread = make_host_guest_thread("gpu");
    const uint32_t ring = phys(g_gpu.ringBase), dw = (8u << g_gpu.ringLog2) / 4, mask = dw - 1;
    LOG("[gpu] ring %08X, %u dwords\n", ring, dw);
    uint32_t rptr = 0; int idle = 0;
    std::vector<uint32_t> buf;
    for (;;) {
        uint32_t wptr = rd32(reg_addr(0x01C5)) & mask;   // CP_RB_WPTR (0x7FC80714)
        if (wptr == rptr) {
            if (idle < 2000) { idle++; std::this_thread::yield(); } else Sleep(1);
            continue;
        }
        idle = 0;
        uint32_t pend = (wptr - rptr) & mask;
        buf.resize(pend);
        for (uint32_t k = 0; k < pend; k++) buf[k] = rd32(ring + ((rptr + k) & mask) * 4);
        execute(buf.data(), pend, 0);
        rptr = wptr;
        wr32(reg_addr(0x01C4), rptr);                     // CP_RB_RPTR
        if (g_gpu.wbAddr) wr32(phys(g_gpu.wbAddr), rptr); // read pointer write-back
    }
}
static void vsync_thread() {
    g_vsyncThread = make_host_guest_thread("vsync");
    auto next = std::chrono::steady_clock::now();
    uint64_t n = 0;
    // ASURA_VBLANKHZ=<hz> (experiment, default 60): the rate of the vblank interrupt. The game presents every second vblank, so 120 asks it for 60 fps.
    static const int kHz = getenv("ASURA_VBLANKHZ") ? std::max(atoi(getenv("ASURA_VBLANKHZ")), 1) : 60;
    const auto period = std::chrono::microseconds(1000000 / kHz);
    if (kHz != 60) LOG("[gpu] vblank at %d Hz (ASURA_VBLANKHZ)\n", kHz);
    for (;;) {
        next += period;
        std::this_thread::sleep_until(next);
        if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(50)) next = std::chrono::steady_clock::now();
        wr32(reg_addr(0x1951), 1);                        // D1MODE_VBLANK_STATUS
        wr32(reg_addr(0x194C), (uint32_t)(n * 750));      // D1MODE_V_COUNTER, roughly
        n++;
        call_interrupt(0, 2, g_vsyncThread);
        if (n % 600 == 0 && g_logging) {
            fprintf(stderr, "[gpu] packet types:");
            for (int k = 0; k < 128; k++) if (g_opCount[k]) fprintf(stderr, " %02X:%u", k, g_opCount[k].load());
            fprintf(stderr, "\n");
        }
        if (n % 600 == 0) LOG("[gpu] %llu s: swaps=%llu interrupts=%llu packets=%llu\n", (unsigned long long)(n / 60),
                              (unsigned long long)g_gpu.swaps.load(), (unsigned long long)g_gpu.interrupts.load(), (unsigned long long)g_gpu.packets.load());
    }
}

void gpu_init_mmio() {
    VirtualAlloc(g_base + kMmio, 0x40000, MEM_COMMIT, PAGE_READWRITE);   // registers 0..0xFFFF
    VirtualAlloc(g_base + 0x7FEA0000, 0x10000, MEM_COMMIT, PAGE_READWRITE);   // XMA decoder registers: plain memory until XMA is emulated
    reg_write(0x0F00, 0x08100748);   // RB_EDRAM_TIMING
    reg_write(0x0F01, 0x0000200E);   // RB_BC_CONTROL
    reg_write(0x05D0, 0);            // RBBM_STATUS: idle
}

// ---- video kernel ----
static void VdSetGraphicsInterruptCallback_(PPCContext& ctx, uint8_t*) {
    g_gpu.intrCb = ctx.r3.u32; g_gpu.intrCtx = ctx.r4.u32;
    LOG("[gpu] interrupt callback %08X ctx %08X\n", g_gpu.intrCb, g_gpu.intrCtx);
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) std::thread(vsync_thread).detach();
}
static void VdInitializeRingBuffer_(PPCContext& ctx, uint8_t*) {
    g_gpu.ringBase = ctx.r3.u32; g_gpu.ringLog2 = ctx.r4.u32;
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) std::thread(gpu_thread).detach();
}
static void VdEnableRingBufferRPtrWriteBack_(PPCContext& ctx, uint8_t*) { g_gpu.wbAddr = ctx.r3.u32; LOG("[gpu] rptr write-back %08X\n", ctx.r3.u32); }
static void VdSetSystemCommandBufferGpuIdentifierAddress_(PPCContext& ctx, uint8_t*) { LOG("[gpu] identifier address %08X\n", ctx.r3.u32); }
// VdGetSystemCommandBuffer(buffer*, token*)
static void VdGetSystemCommandBuffer_(PPCContext& ctx, uint8_t*) {
    if (ctx.r3.u32) { memset(g_base + ctx.r3.u32, 0, 0x94); wr32(ctx.r3.u32, 0xBEEF0000); }
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0xBEEF0001);
    ctx.r3.u64 = 0;
}
// VdSwap(buffer*, fetch*, ..., frontbuffer** in r8): fills the reserved 64-dword slot with an XE_SWAP packet
static void VdSwap_(PPCContext& ctx, uint8_t*) {
    uint32_t buf = ctx.r3.u32, front = ctx.r8.u32 ? rd32(ctx.r8.u32) : 0;
    // the GPU gets the physical address, as MmGetPhysicalAddress gives it: the E0 window is 0x1000 ahead (E032E000 -> 0032F000)
    if (front) front = (front & 0x1FFFFFFFu) + (front >= 0xE0000000u ? 0x1000u : 0u);
    if (buf) {   // the layout the renderer expects (as in Xenia and BCR2): 'SWAP', front buffer, then padding
        wr32(buf, 0xC0000000u | (62u << 16) | (0x64u << 8));
        wr32(buf + 4, 0x53574150u);
        wr32(buf + 8, front);
        for (uint32_t i = 3; i < 64; i++) wr32(buf + i * 4, 0);
    }
    ctx.r3.u64 = 0;
}
// VdInitializeScalerCommandBuffer(source xy, source wh, dest xy, dest wh, filter, ..., dest* (11th, r1+0x64), dest dwords (12th, r1+0x6C)): NOPs.
// The caller copies the returned number of dwords from dest into its command buffer.
static void VdInitializeScalerCommandBuffer_(PPCContext& ctx, uint8_t*) {
    uint32_t dst = rd32(ctx.r1.u32 + 0x64), cap = rd32(ctx.r1.u32 + 0x6C), n = std::min<uint32_t>(0x1CC / 4, cap ? cap : 0x1CC / 4);
    if (dst) for (uint32_t i = 0; i < n; i++) wr32(dst + i * 4, 0x80000000u);
    ctx.r3.u64 = n;
}
// VdGetCurrentDisplayInformation(X_DISPLAY_INFO*): 0x58 bytes
static void VdGetCurrentDisplayInformation_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r3.u32;
    if (!p) return;
    memset(g_base + p, 0, 0x58);
    wr16(p, 1280); wr16(p + 2, 720); wr32(p + 0x18, 0x42700000);
    wr16(p + 0x50, 1280); wr16(p + 0x52, 720);
}
static void VdQueryVideoMode_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r3.u32;
    if (!p) return;
    memset(g_base + p, 0, 0x30);
    wr32(p, 1280); wr32(p + 4, 720); wr32(p + 0xC, 1); wr32(p + 0x10, 1); wr32(p + 0x14, 0x42700000); wr32(p + 0x18, 1); wr32(p + 0x1C, 0x4A); wr32(p + 0x20, 1);
}
static void VdQueryVideoFlags_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 3; }
static void VdGetCurrentDisplayGamma_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr32(ctx.r3.u32, 2); if (ctx.r4.u32) wr32(ctx.r4.u32, 0x400CCCCD); }
static void VdIsHSIOTrainingSucceeded_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void Zero_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
static void NoOp_(PPCContext&, uint8_t*) {}

void register_gpu_imports() {
    register_import("VdSetGraphicsInterruptCallback", VdSetGraphicsInterruptCallback_);
    register_import("VdInitializeRingBuffer", VdInitializeRingBuffer_);
    register_import("VdEnableRingBufferRPtrWriteBack", VdEnableRingBufferRPtrWriteBack_);
    register_import("VdSetSystemCommandBufferGpuIdentifierAddress", VdSetSystemCommandBufferGpuIdentifierAddress_);
    register_import("VdGetSystemCommandBuffer", VdGetSystemCommandBuffer_);
    register_import("VdSwap", VdSwap_);
    register_import("VdInitializeScalerCommandBuffer", VdInitializeScalerCommandBuffer_);
    register_import("VdGetCurrentDisplayInformation", VdGetCurrentDisplayInformation_);
    register_import("VdQueryVideoMode", VdQueryVideoMode_);
    register_import("VdQueryVideoFlags", VdQueryVideoFlags_);
    register_import("VdGetCurrentDisplayGamma", VdGetCurrentDisplayGamma_);
    register_import("VdIsHSIOTrainingSucceeded", VdIsHSIOTrainingSucceeded_);
    register_import("VdInitializeEngines", Zero_);
    register_import("VdShutdownEngines", NoOp_);
    register_import("VdSetDisplayMode", Zero_);
    register_import("VdSetDisplayModeOverride", Zero_);
    register_import("VdPersistDisplay", Zero_);
    register_import("VdEnableDisableClockGating", NoOp_);
    register_import("VdCallGraphicsNotificationRoutines", Zero_);
    register_import("VdRetrainEDRAM", Zero_);
    register_import("VdRetrainEDRAMWorker", Zero_);
}
