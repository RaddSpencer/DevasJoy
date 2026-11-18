// XMA decoder emulation. The console decodes XMA (Microsoft's WMA Pro variant, which UE3 and XAudio2 use for nearly all sound)
// in hardware: the game fills in 64-byte contexts in an array in physical memory and starts (kick), stops (lock) and resets
// (clear) them through little-endian registers at 0x7FEA0000 (register n at +4n). Layout as documented by Xenia (src/xenia/apu):
//   0x600 context array physical address, 0x650-0x659 kick, 0x690-0x699 lock, 0x6A0-0x6A9 clear (one bit per context).
// Decoding uses FFmpeg's XMA2 decoder (thirdparty\ffmpeg, LGPL, linked as DLLs), one 2 KB packet at a time; the PCM goes to the
// context's output ring as big-endian 16-bit samples in 256-byte blocks, where XAudio2 picks it up.
//
// Register writes are events on the hardware, but plain stores here, so the register page is kept read-only: a write faults,
// is let through one instruction, and is applied at once (a background scan catches anything that slips past).
#include "kernel.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")

uint32_t gpu_phys_to_host(uint32_t p);   // kernel.cpp
static constexpr uint32_t kRegs = 0x7FEA0000, kContexts = 320, kTrapPage = 0x7FEA1000;
static constexpr uint32_t kRegArray = 0x600, kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0;
static uint32_t g_array = 0;

struct Ctx {
    bool used = false, enabled = false;
    AVCodecContext* dec = nullptr;
    int channels = 0, rate = 0;
    std::vector<int16_t> pending;     // decoded, interleaved, waiting for room in the output ring
    uint32_t lastOffset = 0xFFFFFFFFu;  // the input read offset we last wrote: anything else means the game moved it
    uint32_t skipBits = 0;              // after a loop: the loop start, so the next packet drops the samples before it
    uint64_t packets = 0, blocks = 0;
};
static Ctx g_xma[kContexts];
static std::mutex g_lock;
static const bool kXmaLog = getenv("ASURA_XMALOG") != nullptr;   // each buffer started, looped and handed back, and every kick, lock and clear
uint64_t xma_log_frame();

static inline uint32_t reg_get(uint32_t r) { return *(volatile uint32_t*)(g_base + kRegs + r * 4); }
static inline uint32_t* cword(uint32_t i, uint32_t w) { return (uint32_t*)(g_base + g_array + i * 64 + w * 4); }
static inline uint32_t cget(uint32_t i, uint32_t w) { return _byteswap_ulong(*(volatile uint32_t*)cword(i, w)); }
// change some bits of a context word without losing the game's own concurrent changes to the others
static void cupdate(uint32_t i, uint32_t w, uint32_t mask, uint32_t value) {
    volatile LONG* p = (volatile LONG*)cword(i, w);
    for (;;) {
        LONG cur = *p;
        uint32_t v = (_byteswap_ulong((uint32_t)cur) & ~mask) | (value & mask);
        if (InterlockedCompareExchange(p, (LONG)_byteswap_ulong(v), cur) == cur) return;
    }
}

static void dec_close(Ctx& c) {
    if (c.dec) avcodec_free_context(&c.dec);
    c.pending.clear();
    c.channels = c.rate = 0;
}
static bool dec_open(Ctx& c, int channels, int rate) {
    if (c.dec && c.channels == channels && c.rate == rate) return true;
    dec_close(c);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) { fprintf(stderr, "[xma] FFmpeg has no XMA2 decoder\n"); return false; }
    c.dec = avcodec_alloc_context3(codec);
    av_channel_layout_default(&c.dec->ch_layout, channels);
    c.dec->sample_rate = rate;
    c.dec->block_align = 2048;
    // XMA2WAVEFORMATEX extradata (34 bytes, little-endian): one stream, its channel mask
    c.dec->extradata = (uint8_t*)av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    c.dec->extradata_size = 34;
    c.dec->extradata[0] = 1;
    const uint32_t mask = channels == 2 ? 3u : 4u;
    memcpy(c.dec->extradata + 2, &mask, 4);
    if (avcodec_open2(c.dec, codec, nullptr) < 0) { fprintf(stderr, "[xma] could not open the decoder (%d channels, %d Hz)\n", channels, rate); avcodec_free_context(&c.dec); return false; }
    c.channels = channels; c.rate = rate;
    return true;
}
// one 2048-byte XMA packet in, whatever whole frames come out appended to pending
static void dec_packet(Ctx& c, const uint8_t* data) {
    AVPacket* pkt = av_packet_alloc();
    av_new_packet(pkt, 2048);
    memcpy(pkt->data, data, 2048);
    if (avcodec_send_packet(c.dec, pkt) == 0) {
        AVFrame* f = av_frame_alloc();
        while (avcodec_receive_frame(c.dec, f) == 0) {
            const int n = f->nb_samples, ch = c.channels;
            const size_t at = c.pending.size();
            c.pending.resize(at + (size_t)n * ch);
            for (int s = 0; s < n; s++)
                for (int k = 0; k < ch; k++) {
                    const float* src = (const float*)f->extended_data[f->format == AV_SAMPLE_FMT_FLTP ? k : 0];
                    float v = f->format == AV_SAMPLE_FMT_FLTP ? src[s] : src[s * ch + k];
                    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
                    c.pending[at + (size_t)s * ch + k] = (int16_t)(v * 32767.0f);
                }
            av_frame_unref(f);
        }
        av_frame_free(&f);
    }
    av_packet_free(&pkt);
}

// Fields (each context word big-endian; bit 0 is the least significant), from Xenia's XMA_CONTEXT_DATA:
//   w0: input 0 packets 0-11, loop count 12-19, input 0 valid 20, input 1 valid 21, output blocks 22-26, output write offset 27-31
//   w1: input 1 packets 0-11, sample rate 27-28 (24/32/44.1/48 kHz), stereo 29, output valid 31
//   w2: input read offset in bits 0-25;  w3: loop start (bits) 0-25;  w4: loop end (bits) 0-25, current input buffer 31
//   w5/w6: input 0/1 physical address;  w7: output physical address;  w9: output read offset 0-4 (blocks)
static void work(uint32_t i) {
    Ctx& c = g_xma[i];
    uint32_t w0 = cget(i, 0), w1 = cget(i, 1);
    if (!(w1 >> 31)) return;                                   // no output buffer
    const uint32_t blocks = (w0 >> 22) & 31;
    if (!blocks) return;
    static const int kRates[4] = { 24000, 32000, 44100, 48000 };
    const int channels = ((w1 >> 29) & 1) ? 2 : 1, rate = kRates[(w1 >> 27) & 3];
    if (!dec_open(c, channels, rate)) return;
    const uint32_t samplesPerBlock = 128;                      // 256 bytes of 16-bit samples
    uint32_t wr = (w0 >> 27) & 31;
    const uint32_t rd = cget(i, 9) & 31;
    uint32_t used = (wr + blocks - rd) % blocks, room = blocks - 1 - used;
    const uint32_t out = gpu_phys_to_host(cget(i, 7));
    for (int guard = 0; room > 0 && guard < 64; guard++) {
        if (c.pending.size() >= samplesPerBlock) {             // one block out
            uint16_t* dst = (uint16_t*)(g_base + out + wr * 256);
            for (uint32_t s = 0; s < samplesPerBlock; s++) dst[s] = _byteswap_ushort((uint16_t)c.pending[s]);
            c.pending.erase(c.pending.begin(), c.pending.begin() + samplesPerBlock);
            wr = (wr + 1) % blocks; room--; c.blocks++;
            cupdate(i, 0, 31u << 27, wr << 27);
            continue;
        }
        // more input: the current buffer, or the other one when it's used up
        w0 = cget(i, 0);
        uint32_t w4 = cget(i, 4), cur = w4 >> 31;
        uint32_t offset = cget(i, 2) & 0x3FFFFFF;
        if (offset != c.lastOffset && c.lastOffset != 0xFFFFFFFFu) { avcodec_flush_buffers(c.dec); c.pending.clear(); }   // the game moved it
        bool valid = (w0 >> (20 + cur)) & 1;
        if (!valid) {
            if (!((w0 >> (21 - cur)) & 1)) break;              // neither buffer has data
            cur ^= 1; offset = 32;
            cupdate(i, 4, 1u << 31, cur << 31);
            cupdate(i, 2, 0x3FFFFFF, offset);
        }
        const uint32_t count = cur ? (cget(i, 1) & 0xFFF) : (w0 & 0xFFF);
        const uint32_t base = cget(i, 5 + cur);
        uint32_t packet = offset / (2048 * 8);
        if (packet >= count) {                                  // this buffer is done: hand it back, go on with the other
            cupdate(i, 0, 1u << (20 + cur), 0);
            c.lastOffset = 0xFFFFFFFFu;
            if (!((cget(i, 0) >> (21 - cur)) & 1)) { cupdate(i, 2, 0x3FFFFFF, 32); c.lastOffset = 32; break; }
            cur ^= 1;
            cupdate(i, 4, 1u << 31, cur << 31);
            cupdate(i, 2, 0x3FFFFFF, 32); c.lastOffset = 32;
            continue;
        }
        if (kXmaLog && packet == 0)
            fprintf(stderr, "[xmalog] f=%llu ctx %u start buf %u at %08X packets %u loops %u loop %u-%u ch %d rate %d\n", (unsigned long long)xma_log_frame(), i, cur, base, count,
                    (w0 >> 12) & 0xFF, cget(i, 3) & 0x3FFFFFF, w4 & 0x3FFFFFF, channels, rate);
        const size_t before = c.pending.size();
        dec_packet(c, g_base + gpu_phys_to_host(base) + packet * 2048);
        c.packets++;
        offset = (packet + 1) * 2048 * 8 + 32;
        // hardware looping: from loop end back to loop start, loop count times (255 = for ever)
        const uint32_t loops = (w0 >> 12) & 0xFF, loopEnd = w4 & 0x3FFFFFF, loopStart = cget(i, 3) & 0x3FFFFFF;
        {   // Loop points are bit offsets and usually fall inside a 2 KB packet, but packets are decoded whole: the loop's last packet
            // played to its end (episode 8's crowd loop: ~9% extra per pass, a repeated fragment of speech each time round, on five
            // emitters at once), and a loop start inside a packet played that packet's beginning again. Keep only the share of this
            // packet's samples inside the loop (frames are close to evenly spread over a packet). ASURA_XMALOOPFULL=1: whole packets.
            static const bool kFull = getenv("ASURA_XMALOOPFULL") != nullptr;
            const uint32_t pStart = packet * 2048 * 8 + 32, pBits = 2048 * 8 - 32;
            const size_t got = c.pending.size() - before, ch = (size_t)std::max(c.channels, 1);
            if (!kFull && got >= ch) {
                size_t from = 0, to = got / ch;
                if (c.skipBits && c.skipBits > pStart && c.skipBits < pStart + pBits) from = (size_t)((double)(c.skipBits - pStart) / pBits * (got / ch));
                if (loops && loopEnd > pStart && loopEnd < offset) to = (size_t)((double)(loopEnd - pStart) / pBits * (got / ch));
                if (from > 0 || to < got / ch) {
                    if (to < from) to = from;
                    std::vector<int16_t> keep(c.pending.begin() + before + from * ch, c.pending.begin() + before + to * ch);
                    c.pending.resize(before); c.pending.insert(c.pending.end(), keep.begin(), keep.end());
                }
            }
            c.skipBits = 0;
        }
        if (loops && loopEnd && offset >= loopEnd) {
            c.skipBits = loopStart;                                         // the loop start's packet: drop what comes before it
            if (kXmaLog) fprintf(stderr, "[xmalog] f=%llu ctx %u loop %u -> %u, count %u\n", (unsigned long long)xma_log_frame(), i, offset, loopStart, loops);
            offset = loopStart;
            if (loops != 255) cupdate(i, 0, 0xFFu << 12, (loops - 1) << 12);
            avcodec_flush_buffers(c.dec);
        }
        cupdate(i, 2, 0x3FFFFFF, offset);
        c.lastOffset = offset;
    }
}

// ---- registers ----
static void apply_bank(uint32_t reg, int what) {
    for (uint32_t b = 0; b < 10; b++) {
        volatile LONG* p = (volatile LONG*)(g_base + kRegs + (reg + b) * 4);
        uint32_t bits = (uint32_t)InterlockedExchange(p, 0);   // little-endian: the host order
        for (; bits; bits &= bits - 1) {
            unsigned long k; _BitScanForward(&k, bits);
            uint32_t i = b * 32 + k;
            if (i >= kContexts) continue;
            Ctx& c = g_xma[i];
            if (what == 0) c.enabled = true;
            else if (what == 1) c.enabled = false;
            else { c.enabled = false; if (c.dec) avcodec_flush_buffers(c.dec); c.pending.clear(); c.lastOffset = 0xFFFFFFFFu; }
            static int n = 0;
            if (kXmaLog) fprintf(stderr, "[xmalog] f=%llu ctx %u %s\n", (unsigned long long)xma_log_frame(), i, what == 0 ? "kick" : what == 1 ? "lock" : "clear");
            else if (n++ < 60) LOG("[xma] %s context %u\n", what == 0 ? "kick" : what == 1 ? "lock" : "clear", i);
        }
    }
}
// The register page is read-only between writes. Lock order: g_trap (page protection) before g_lock (context state).
// A game thread holds g_trap from its faulting store until the single step after it; the worker takes it to clear bits.
static CRITICAL_SECTION g_trap;
static void apply_all() {   // page writable, g_trap held
    std::lock_guard<std::mutex> g(g_lock);
    apply_bank(kRegClear, 2);
    apply_bank(kRegLock, 1);
    apply_bank(kRegKick, 0);
}
static bool any_pending() {
    for (uint32_t b = 0; b < 10; b++) if (reg_get(kRegKick + b) | reg_get(kRegLock + b) | reg_get(kRegClear + b)) return true;
    return false;
}
static void scan_registers() {   // worker: anything a trapped write didn't handle (two threads writing at once)
    if (!any_pending()) return;
    EnterCriticalSection(&g_trap);
    DWORD old; VirtualProtect(g_base + kTrapPage, 0x1000, PAGE_READWRITE, &old);
    apply_all();
    VirtualProtect(g_base + kTrapPage, 0x1000, PAGE_READONLY, &old);
    LeaveCriticalSection(&g_trap);
}
static thread_local bool t_trapStep = false;
static LONG CALLBACK trap_handler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (g_base && a >= g_base + kTrapPage && a < g_base + kTrapPage + 0x1000 && !t_trapStep) {
            EnterCriticalSection(&g_trap);
            DWORD old; VirtualProtect(g_base + kTrapPage, 0x1000, PAGE_READWRITE, &old);
            ep->ContextRecord->EFlags |= 0x100;               // single-step over the store, then handle it
            t_trapStep = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && t_trapStep) {
        t_trapStep = false;
        apply_all();
        DWORD old; VirtualProtect(g_base + kTrapPage, 0x1000, PAGE_READONLY, &old);
        LeaveCriticalSection(&g_trap);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void worker() {
    for (;;) {
        Sleep(1);
        scan_registers();
        std::lock_guard<std::mutex> g(g_lock);
        for (uint32_t i = 0; i < kContexts; i++) if (g_xma[i].used && g_xma[i].enabled) work(i);
        static ULONGLONG lastLog = 0;
        if (g_logging && GetTickCount64() - lastLog > 10000) {
            lastLog = GetTickCount64();
            int active = 0; uint64_t pk = 0, bl = 0;
            for (auto& c : g_xma) if (c.used && c.enabled) { active++; pk += c.packets; bl += c.blocks; }
            fprintf(stderr, "[xma] %d contexts running, %llu packets decoded, %llu blocks written\n", active, (unsigned long long)pk, (unsigned long long)bl);
        }
    }
}

void xma_init() {
    if (g_array) return;
    g_array = phys_alloc(kContexts * 64, 0x1000);
    memset(g_base + g_array, 0, kContexts * 64);
    *(uint32_t*)(g_base + kRegs + kRegArray * 4) = g_array & 0x1FFFFFFFu;   // little-endian
    av_log_set_level(AV_LOG_ERROR);
    if (!getenv("ASURA_NOXMA")) {
        InitializeCriticalSection(&g_trap);
        AddVectoredExceptionHandler(1, trap_handler);
        DWORD old; VirtualProtect(g_base + kTrapPage, 0x1000, PAGE_READONLY, &old);
        std::thread(worker).detach();
    }
    LOG("[xma] context array at %08X\n", g_array);
}
// XMACreateContext(context*)
static void XMACreateContext_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_lock);
    uint32_t i = 0;
    while (i < kContexts && g_xma[i].used) i++;
    if (i == kContexts) { fprintf(stderr, "[xma] out of contexts\n"); ctx.r3.u64 = STATUS_NO_MEMORY; return; }
    g_xma[i].used = true; g_xma[i].enabled = false; g_xma[i].lastOffset = 0xFFFFFFFFu;
    memset(g_base + g_array + i * 64, 0, 64);
    if (ctx.r3.u32) wr32(ctx.r3.u32, g_array + i * 64);
    ctx.r3.u64 = 0;
}
// XMAReleaseContext(context)
static void XMAReleaseContext_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_lock);
    uint32_t i = (ctx.r3.u32 - g_array) / 64;
    if (ctx.r3.u32 >= g_array && i < kContexts) { g_xma[i].used = g_xma[i].enabled = false; dec_close(g_xma[i]); }
    ctx.r3.u64 = 0;
}
void register_xma_imports() {
    xma_init();
    register_import("XMACreateContext", XMACreateContext_);
    register_import("XMAReleaseContext", XMAReleaseContext_);
}
