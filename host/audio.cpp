// Audio: the XAudio render driver. The game's client is called whenever the output device has room for another 256-sample frame
// (48 kHz, so about every 5.33 ms; with no device, on a 5.33 ms clock), and the frame it submits goes to WASAPI (audio_out.cpp,
// from BCR2). XMA decoding is in xma.cpp.
#include "kernel.h"
#include "audio_out.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

void guest_fp_init(PPCContext& ctx);
static std::atomic<uint32_t> g_cb{0}, g_arg{0};
static std::atomic<uint64_t> g_frames{0};

static void audio_thread() {
    GuestHostThread t = make_host_guest_thread("audio");
    const uint32_t argWord = vm_alloc(0x1000);
    const auto kFrame = std::chrono::microseconds(5333);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        uint32_t cb = g_cb.load();
        if (!cb) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); next = std::chrono::steady_clock::now(); continue; }
        const auto now = std::chrono::steady_clock::now();
        const int room = audio_out_room();               // frames the device wants now; -1 without a device (keep our own clock)
        if (room == 0 || (room < 0 && now < next)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        next += kFrame;
        if (next < now - std::chrono::milliseconds(100)) next = now;   // no catching up after a long stall
        PPCContext c{}; guest_fp_init(c);
        bind_guest_thread(c, t);
        wr32(argWord, g_arg.load());
        c.r3.u64 = argWord;                      // the client reads its argument through r3 (see the BCR2 notes)
        call_guest(c, cb);
        uint64_t n = ++g_frames;
        if (n == 1 || n % 11250 == 0) LOG("[audio] frame %llu\n", (unsigned long long)n);
    }
}
// XAudioRegisterRenderDriverClient({u32 callback; u32 arg}*, driver*)
static void XAudioRegisterRenderDriverClient_(PPCContext& ctx, uint8_t*) {
    uint32_t s = ctx.r3.u32, cb = s ? rd32(s) : 0, arg = s ? rd32(s + 4) : 0;
    LOG("[audio] client callback %08X arg %08X\n", cb, arg);
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0x41550001);
    if (cb && !getenv("ASURA_NOAUDIO")) {
        g_arg = arg; g_cb = cb;
        static std::atomic<bool> started{false};
        if (!started.exchange(true)) std::thread(audio_thread).detach();
    }
    ctx.r3.u64 = 0;
}
static void XAudioUnregisterRenderDriverClient_(PPCContext& ctx, uint8_t*) { g_cb = 0; ctx.r3.u64 = 0; }
// XAudioSubmitRenderDriverFrame(driver, samples*): 6 channels x 256 big-endian floats, from inside the client callback on the audio thread
static void XAudioSubmitRenderDriverFrame_(PPCContext& ctx, uint8_t*) { if (ctx.r4.u32) audio_out_submit(g_base + ctx.r4.u32); ctx.r3.u64 = 0; }
static void XAudioGetSpeakerConfig_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr32(ctx.r3.u32, 0x00010001); ctx.r3.u64 = 0; }
static void XAudioGetVoiceCategoryVolume_(PPCContext& ctx, uint8_t*) { if (ctx.r4.u32) wr32(ctx.r4.u32, 0x3F800000); ctx.r3.u64 = 0; }
static void XAudioGetDucker_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }

void register_audio_imports() {
    register_import("XAudioRegisterRenderDriverClient", XAudioRegisterRenderDriverClient_);
    register_import("XAudioUnregisterRenderDriverClient", XAudioUnregisterRenderDriverClient_);
    register_import("XAudioSubmitRenderDriverFrame", XAudioSubmitRenderDriverFrame_);
    register_import("XAudioGetSpeakerConfig", XAudioGetSpeakerConfig_);
    register_import("XAudioGetVoiceCategoryVolume", XAudioGetVoiceCategoryVolume_);
    for (const char* n : { "XAudioEnableDucker", "XAudioGetDuckerLevel", "XAudioGetDuckerThreshold", "XAudioGetDuckerAttackTime",
                           "XAudioGetDuckerReleaseTime", "XAudioGetDuckerHoldTime" })
        register_import(n, XAudioGetDucker_);
}
