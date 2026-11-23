// Xenos (Xbox 360 GPU) renderer on Direct3D 11. Ported from the BCR2 host (C:\dev\bcr2\host\gpu.cpp) by tools\port_renderer.py:
// keep it close to that file so the two can merge into one shared renderer. Asura-specific glue is in xenos_glue.cpp.
// D3D11 renderer for the Xenos PM4 stream.
//
// Overview
//  - gpu_exec (imports.cpp) feeds register writes and predicated-in type-3 packets here, on the GPU thread.
//  - Shaders: Xenos microcode is translated to HLSL text (translate_shader) and compiled with D3DCompile; results are
//    cached on disk in <exe dir>\shadercache. All stages share a fixed 16-interpolator signature.
//  - Vertices: vfetch is done in the vertex shader from a ByteAddressBuffer ring holding the byte-swapped guest data.
//  - EDRAM: each (base, format, pitch) gets its own host render target, no aliasing between formats and no MSAA.
//    Tiled rendering works because every tile hits the same host target through its window offset.
//  - Resolves: copied to a host "surface" keyed by guest destination address, never written back to guest memory.
//    Texture lookups and VdSwap find them by address.
//
// Env switches: BCR2_NOGPU (null GPU), BCR2_GPUTRACE=n (log first n draws/resolves), BCR2_SHOTDIR + BCR2_SHOTEVERY (BMP
// frame dumps), BCR2_SHADERDUMP=dir (write HLSL), BCR2_GPUWIDE=1 (log every unknown format once, on by default).
#define NOMINMAX
#include "gpu.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include "shaderpack.h"
#include "inputguard.h"
#include "shaderindex.h"
#include "paths.h"
#include "kbm.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <wincodec.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

// ---- registers ----
enum : uint32_t {
    R_SURFACE_INFO = 0x2000, R_COLOR_INFO = 0x2001, R_DEPTH_INFO = 0x2002, R_COLOR1_INFO = 0x2003, R_COLOR2_INFO = 0x2004, R_COLOR3_INFO = 0x2005,
    R_SCREEN_TL = 0x200E, R_SCREEN_BR = 0x200F, R_WINDOW_OFFSET = 0x2080, R_WINDOW_TL = 0x2081, R_WINDOW_BR = 0x2082,
    R_INDX_OFFSET = 0x2102, R_RESET_INDX = 0x2103, R_COLOR_MASK = 0x2104, R_BLEND_RED = 0x2105,
    R_STENCIL_BF = 0x210C, R_STENCIL = 0x210D, R_ALPHA_REF = 0x210E, R_VPORT_XSCALE = 0x210F,
    R_PROGRAM_CNTL = 0x2180, R_CONTEXT_MISC = 0x2181,
    R_DEPTHCONTROL = 0x2200, R_BLEND0 = 0x2201, R_COLORCONTROL = 0x2202, R_CLIP_CNTL = 0x2204, R_MODE_CNTL = 0x2205, R_VTE_CNTL = 0x2206,
    R_MODECONTROL = 0x2208, R_BLEND1 = 0x2209, R_VTX_CNTL = 0x2302,
    R_COPY_CONTROL = 0x2318, R_COPY_DEST_BASE = 0x2319, R_COPY_DEST_PITCH = 0x231A, R_COPY_DEST_INFO = 0x231B,
    R_DEPTH_CLEAR = 0x231D, R_COLOR_CLEAR = 0x231E, R_COLOR_CLEAR_LO = 0x231F,
    R_OFFSET_FRONT_SCALE = 0x2380,
    R_DMA_BASE = 0x21FA, R_DMA_SIZE = 0x21FB,
    R_CONST = 0x4000, R_FETCH = 0x4800, R_BOOL = 0x4900, R_LOOP = 0x4908,
};

static uint32_t g_regs[0x10000];
static bool g_constDirty = true;
// Display gamma ramp the game programs into the display controller. Scanout applies it, so presenting must too.
static uint32_t g_lutEntries[256], g_lutIndex = 0;
static bool g_lutDirty = false, g_lutSet = false;
static uint8_t* g_base = nullptr;
static int g_trace = 0;

bool gpu_enabled() { static const bool on = getenv("ASURA_NOGPU") == nullptr; return on; }
bool gpu_tracing() { return g_trace > 1000; }

// ---- frame timing: printed as [perf] every 300 swaps ----
static double g_perfTicksToMs = 0;
static double g_tDraw = 0, g_tResolve = 0, g_tTex = 0, g_tPresent = 0;
static std::atomic<uint64_t> g_waitTicks[3];                                // ring empty, WAIT_REG_MEM, interrupt
static std::atomic<uint64_t> g_nCompiles{0};
static thread_local bool t_warmThread = false;                                // a pixel-shader warm-up thread (not counted as the game's compiles)
static uint64_t g_nUploads = 0, g_upBytes = 0;
void gpu_note_wait(int kind, uint64_t qpcTicks) { g_waitTicks[kind] += qpcTicks; }
// Parts of the slowest draw and of the swap, for the [spike] line.
static double g_sCb = 0, g_sVb = 0, g_sIb = 0, g_sCall = 0, g_sRt = 0, g_swPrep = 0, g_swHold = 0, g_swTrack = 0;
struct PerfScope {
    double& acc; LARGE_INTEGER a;
    explicit PerfScope(double& t) : acc(t) { QueryPerformanceCounter(&a); }
    ~PerfScope() { LARGE_INTEGER b; QueryPerformanceCounter(&b); acc += (double)(b.QuadPart - a.QuadPart) * g_perfTicksToMs; }
};

void gpu_reg(uint32_t r, uint32_t v) {
    if (r >= 0x10000) return;
    {   // ASURA_CONSTWATCH: every write to the watched constant, however it arrives (type-0 register writes included), while tracing
        static const int kW = getenv("ASURA_CONSTWATCH") ? atoi(getenv("ASURA_CONSTWATCH")) : -1;
        if (kW >= 0 && g_trace > 0 && r >= R_CONST + kW * 4u && r < R_CONST + kW * 4u + 4) {
            float f; memcpy(&f, &v, 4);
            fprintf(stderr, "[constreg] c%d.%c = %g (%08X)\n", kW, "xyzw"[r - R_CONST - kW * 4], f, v);
        }
    }
    g_regs[r] = v;
    if (r >= R_CONST && r < R_CONST + 0x800) g_constDirty = true;
    if (r == 0x1922) g_lutIndex = v & 0xFF;                                 // DC_LUT_RW_INDEX
    else if (r == 0x1925) {                                                 // DC_LUT_30_COLOR: 10 bits each, red high, auto-increments
        g_lutEntries[g_lutIndex] = v;
        g_lutIndex = (g_lutIndex + 1) & 0xFF;
        g_lutDirty = g_lutSet = true;
    }
    if (r >= 0x1900 && r < 0x1B00 && r != 0x1925) {                         // other display controller registers, for reference
        static int shown = 0;
        if (shown++ < 40) fprintf(stderr, "[dc] reg %04X = %08X\n", r, v);
    }
}

static inline float reg_f(uint32_t r) { float f; memcpy(&f, &g_regs[r], 4); return f; }
uint32_t gpu_phys_to_host(uint32_t p);   // kernel.cpp: physical address -> guest address (E0 window pages are 0x1000 behind)
static inline const uint8_t* phys(uint32_t a) { return g_base + gpu_phys_to_host(a); }
static inline int sext(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }

static void logf_once(const char* key, const char* fmt, ...) {
    static std::set<std::string> seen;
    if (!seen.insert(key).second) return;
    va_list a; va_start(a, fmt); fprintf(stderr, "[gfx] "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a);
}
static std::string S(const char* fmt, ...) {
    char buf[1024]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); return buf;
}
static uint64_t fnv64(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// ---- device ----
static HWND g_hwnd = nullptr;
static std::atomic<int> g_shotBurstLeft{0};
static std::atomic<int> g_shotStep{1};
static std::atomic<bool> g_ringDump{false};
void capture_request() { g_ringDump = true; fprintf(stderr, "[capture] requested by the input replay\n"); }   // as if F9 were pressed (xam.cpp, ASURA_INPUTCAPTURE)
bool bcr2_log_requested();   // main.cpp: true when --log was given, also before main() runs
static const int g_shotRingSize = getenv("ASURA_SHOTRING") ? atoi(getenv("ASURA_SHOTRING")) : (bcr2_log_requested() ? 150 : 0);   // with --log, F9 saves 150 frames (every frame): 3.5 s before the press and 1.5 s after
static uint64_t g_frame = 0;
uint64_t xma_log_frame() { return g_frame; }
std::atomic<int64_t> g_shotFileFrame{-1};                                  // ASURA_SHOTAFTERFILE: the frame its file was opened (kfile.cpp)                               // the frame number for audio logs (xma.cpp)
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID3D11DeviceContext1> g_ctx1;
static D3D_FEATURE_LEVEL g_featureLevel = D3D_FEATURE_LEVEL_11_0;           // 11_1 lets vertex shaders write UAVs (memexport)
static uint64_t g_mxEpoch = 0;                                               // bumped when memexport writes guest memory: textures re-check their data
static ComPtr<IDXGISwapChain> g_swap;
static ComPtr<ID3D11RenderTargetView> g_backRtv;
static ComPtr<ID3D11Texture2D> g_backTex;
// The swap chain is 10 bits per channel: the game's display gamma ramp (a 10-bit table, as on the console) is applied in the present pass, and an 8-bit
// back buffer rounded its output again, merging neighbouring dark tones into visible bands. ASURA_BACKBUFFER8=1 keeps the old 8-bit chain.
static const DXGI_FORMAT g_backFmt = getenv("ASURA_BACKBUFFER8") ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM;
static ComPtr<ID3D11SamplerState> g_presentSampler;
static ComPtr<ID3D11PixelShader> g_psPresentPlain;
static uint32_t g_backW = 1280, g_backH = 720;
// Player settings (bcr2.ini) become BCR2_* switches here, before the render scale below and the display code read them. Statics in one file initialise in order.
void settings_apply_to_env();
static const bool g_settingsLoaded = (settings_apply_to_env(), true);
// Renders at a whole multiple of the game's 1280x720: by default the smallest one that covers the display (2 at 1440p, 3 at 4K), or BCR2_RESSCALE=n.
// Every size kept in the renderer (Rt::w, Surface::w, resolve rectangles) stays in guest pixels, and only what reaches D3D is multiplied: texture sizes,
// viewports, scissors, clears and resolve copies. Chosen once at launch.
static const int g_resScale = [] {
    int s = 0;
    if (const char* e = getenv("ASURA_RESSCALE")) s = atoi(e);
    if (s < 1) {
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        s = 1;
        const int outH = getenv("ASURA_OUTRES") ? atoi(getenv("ASURA_OUTRES")) : 0;   // Output Resolution chosen: cover that instead
        if (outH >= 360 && outH <= 4320) s = (outH + 719) / 720;
        else if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) s = (int)std::max((dm.dmPelsWidth + 1279) / 1280, (dm.dmPelsHeight + 719) / 720);
    }
    s = s < 1 ? 1 : s > 4 ? 4 : s;
    if (bcr2_log_requested()) fprintf(stderr, "[display] render scale %dx (%dx%d)\n", s, 1280 * s, 720 * s);
    return s;
}();
int gpu_render_scale() { return g_resScale; }
// Vsync (Graphics Options, asura.ini, or ASURA_VSYNC=1): present on the display's refresh instead of as soon as a frame is ready.
static std::atomic<bool> g_vsync{ getenv("ASURA_VSYNC") && atoi(getenv("ASURA_VSYNC")) != 0 };
void gpu_set_vsync(bool on) { g_vsync = on; fprintf(stderr, "[display] vsync %s\n", on ? "on" : "off"); }
static bool g_devOk = false, g_noOverwriteSrv = false;
static ComPtr<ID3D11Buffer> g_cbFloat, g_cbState, g_cbBlit, g_vbRing, g_ibRing;
static ComPtr<ID3D11ShaderResourceView> g_vbSrv;
static ComPtr<ID3D11GeometryShader> g_gsRect;
static ComPtr<ID3D11VertexShader> g_vsBlit;
static ComPtr<ID3D11PixelShader> g_psBlit, g_psBlitDepth, g_psBlitRaw, g_psColorToDepth, g_psPresentLut;
static ComPtr<ID3D11Texture2D> g_lutTex;
static ComPtr<ID3D11ShaderResourceView> g_lutSrv;
static ComPtr<ID3D11SamplerState> g_blitSampler;
static const uint32_t kVbRingSize = 64u << 20, kIbRingSize = 16u << 20;
static const int kVbRings = 3;
static ComPtr<ID3D11Buffer> g_vbRings[kVbRings];
static ComPtr<ID3D11ShaderResourceView> g_vbSrvs[kVbRings];
static ComPtr<ID3D11Query> g_vbDone[kVbRings];                               // signalled when the GPU has finished with that ring's last contents
static bool g_vbUsed[kVbRings] = {};
static int g_vbIndex = 0;
static uint32_t g_vbPos = 0, g_ibPos = 0;
static uint64_t g_vbGen = 0;                                                // bumped whenever vertex ring data can be overwritten or the buffer changes (vertex cache in issue_draw)
static uint64_t g_draws = 0, g_resolves = 0, g_drawsFrame = 0;

// Display modes. The window thread only asks; the GPU thread owns the swap chain and makes the change between frames.
// Alt+Enter or F11: windowed fullscreen. BCR2_DISPLAY=window|borderless picks the starting mode. (Exclusive fullscreen was removed.)
enum DisplayMode { kWindowed = 0, kBorderless = 1, kExclusive = 2 };
static std::atomic<int> g_modeWanted{kWindowed};
static std::atomic<bool> g_sizeChanged{getenv("ASURA_BACKBUFFER") != nullptr};   // a forced back buffer size applies from the start
static int g_modeNow = kWindowed;
int gpu_display_mode() { return g_modeWanted.load(); }
void gpu_set_display_mode(int mode) { g_modeWanted = mode <= 0 ? kWindowed : kBorderless; }   // exclusive fullscreen was removed: it misbehaved when switching back to a window
// The player's gamma (percent, 100 = unchanged) and whether the gamma screen's sample picture is showing. The final pass reads both.
static std::atomic<float> g_gamma{1.0f};
static std::atomic<bool> g_gammaPreview{false};
void gpu_set_gamma(int percent) { g_gamma = (percent < 50 ? 50 : percent > 200 ? 200 : percent) / 100.0f; }
void gpu_set_gamma_preview(bool on) { g_gammaPreview = on; }
// Texture filtering the player picked (0 = as the game asks, 1 bilinear, 2 trilinear, 3 to 6 anisotropic 2x 4x 8x 16x) and the antialiasing pass (0 off, 1 FXAA, 2 FXAA high).
static std::atomic<int> g_texFilter{0};
static std::atomic<int> g_aaMode{0};
void gpu_set_texfilter(int mode) { g_texFilter = mode < 0 ? 0 : mode > 6 ? 6 : mode; }
void gpu_set_aa(int mode) { g_aaMode = mode < 0 ? 0 : mode > 2 ? 2 : mode; }
// Frames per second and average frame time over the last quarter second, for the FPS display. Written by the render thread at each present.
static std::atomic<float> g_fpsNow{0.0f}, g_msNow{0.0f};
std::string gpu_fps_text() { char b[48]; snprintf(b, sizeof b, "%.0f FPS  %.1f ms", g_fpsNow.load(), g_msNow.load()); return b; }
static void note_present_for_fps() {
    static LARGE_INTEGER last{}, freq{}; static int frames = 0;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); return; }
    frames++;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
    if (dt >= 0.5) { g_fpsNow = (float)(frames / dt); g_msNow = (float)(1000.0 * dt / frames); frames = 0; last = now; }
}

extern bool g_logging;   // main.cpp: --log (or BCR2_LOG=1). Off by default: no log output, no shader recording, no F9 capture.
static std::atomic<uint64_t> g_lastF9{0};
static std::atomic<bool> g_screenshotWanted{false};   // F12
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { fprintf(stderr, "[gfx] window closed, exiting\n"); fflush(stderr); void video_finish(); video_finish(); TerminateProcess(GetCurrentProcess(), 0); }
    kbm_note_message(h, m, w, l);
    if (m == WM_SIZE) g_sizeChanged = true;
    if (m == WM_MOUSEWHEEL) { kbm_note_wheel((short)HIWORD(w)); return 0; }
    if (m == WM_KEYDOWN && w == VK_F12 && !(l & (1 << 30))) g_screenshotWanted = true;
    // F9 (only with --log and BCR2_SHOTDIR set): save the last few seconds if BCR2_SHOTRING is set, else about the next 3 seconds, to catch something seen during play.
    if (m == WM_KEYDOWN && w == VK_F9 && !(l & (1 << 30)) && g_logging) { if (g_shotRingSize > 0) g_ringDump = true; else { g_shotBurstLeft = 180; g_shotStep = 3; } g_lastF9 = g_frame; fprintf(stderr, "[capture] F9 pressed at frame %llu\n", (unsigned long long)g_frame); }
    // F8: a marker in the log, nothing else (no capture, no stall), so a tester can say "marks 3-5" instead of a video time.
    if (m == WM_KEYDOWN && w == VK_F8 && !(l & (1 << 30))) {
        static int marks = 0;
        uint32_t input_clock_ms();
        fprintf(stderr, "[mark] %d at frame %llu (input clock %u ms)\n", ++marks, (unsigned long long)g_frame, input_clock_ms());
        fflush(stderr);
    }
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_modeWanted.load() != kWindowed) { SetCursor(nullptr); return TRUE; }
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) || (m == WM_KEYDOWN && w == VK_F11)) {
        if (!(l & (1 << 30))) {                                             // ignore auto-repeat
            int want = kBorderless;
            g_modeWanted = g_modeWanted.load() == kWindowed ? want : kWindowed;
        }
        return 0;
    }
    if (m == WM_SYSCHAR && w == VK_RETURN) return 0;                        // no error beep
    return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI window_thread(LPVOID ev) {
    WNDCLASSW wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"asurawin";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // BCR2_OUTRES=<height>: the size of the picture on screen (1080 is 1920x1080). The game still renders at its own internal scale and is resampled to this.
    int ow = 1280, oh = 720;
    if (const char* e = getenv("ASURA_OUTRES")) { int h = atoi(e); if (h >= 360 && h <= 4320) { oh = h; ow = (h * 16 + 4) / 9; } }
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (ow > (work.right - work.left) - 40 || oh > (work.bottom - work.top) - 80) {   // a window bigger than the screen would be useless: fit it
        const float k = std::min(((work.right - work.left) - 40) / (float)ow, ((work.bottom - work.top) - 80) / (float)oh);
        ow = (int)(ow * k); oh = (int)(oh * k);
    }
    RECT rc{ 0, 0, ow, oh }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    static const bool kMinimized = getenv("ASURA_MINIMIZED") != nullptr;    // test runs: start minimized, without taking focus
    g_hwnd = CreateWindowW(L"asurawin", L"Asura's Wrath (recomp)", WS_OVERLAPPEDWINDOW | (kMinimized ? 0 : WS_VISIBLE), CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (kMinimized) ShowWindow(g_hwnd, SW_SHOWMINNOACTIVE);
    input_guard_install(true);   // Steam's controller chords and other remappers must not Alt+Tab us away (inputguard.h); this thread's loop runs the hooks
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

// Compiled shaders on disk: <exe dir>\shadercache, or BCR2_SHADERCACHE=<dir> to test a cold start without touching the real one.
static const std::string& shader_cache_dir() {
    static const std::string dir = [] {
        if (const char* e = getenv("ASURA_SHADERCACHE")) { CreateDirectoryA(e, nullptr); return std::string(e); }
        wchar_t w[MAX_PATH]; GetModuleFileNameW(nullptr, w, MAX_PATH);
        char a[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, w, -1, a, MAX_PATH, nullptr, nullptr);
        std::string s = a; s = s.substr(0, s.find_last_of('\\')) + "\\shadercache";
        CreateDirectoryA(s.c_str(), nullptr);
        return s;
    }();
    return dir;
}
static ComPtr<ID3DBlob> load_cached(const std::string& src, const char* target) {
    uint64_t h = fnv64(src.data(), src.size()); h = fnv64(target, strlen(target), h);
    std::string path = shader_cache_dir() + "\\" + S("%016llX.dxbc", (unsigned long long)h);
    ComPtr<ID3DBlob> blob;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0 && SUCCEEDED(D3DCreateBlob(n, &blob))) { if (fread(blob->GetBufferPointer(), 1, n, f) != (size_t)n) blob.Reset(); }
        fclose(f);
    }
    return blob;
}
static ComPtr<ID3DBlob> compile_cached(const std::string& src, const char* entry, const char* target) {
    const std::string& dir = shader_cache_dir();
    uint64_t h = fnv64(src.data(), src.size()); h = fnv64(target, strlen(target), h);
    std::string path = dir + "\\" + S("%016llX.dxbc", (unsigned long long)h);
    ComPtr<ID3DBlob> blob = load_cached(src, target);
    if (blob) return blob;
    ComPtr<ID3DBlob> err;
    if (!t_warmThread) g_nCompiles++;                                       // the [spike] count is the game's own compiles
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_ENABLE_STRICTNESS * 0, 0, &blob, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[gfx] shader compile failed (%s): %s\n", target, err ? (const char*)err->GetBufferPointer() : "?");
        static int nd = 0;
        if (nd++ < 8) {
            std::string dp = dir + "\\" + S("failed_%016llX.hlsl", (unsigned long long)h);
            if (FILE* f = fopen(dp.c_str(), "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); fprintf(stderr, "[gfx]   source written to %s\n", dp.c_str()); }
        }
        return nullptr;
    }
    {   // written under a temporary name and renamed: another thread (the warm-up, a compile worker) may read the same file at any moment
        const std::string tmp = path + S(".%lu.tmp", GetCurrentThreadId());
        bool ok = false;
        if (FILE* f = fopen(tmp.c_str(), "wb")) { ok = fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f) == blob->GetBufferSize(); ok = (fclose(f) == 0) && ok; }
        if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
    }
    return blob;
}

static const char* kVOut = "struct VOut { float4 pos : SV_Position; float4 o[16] : TEXCOORD; };\n";
// Vertex fetch decoding with the format and flags given at draw time (generic vertex shaders). The same arithmetic as emit_vfetch's per-format code.
static const char* kVfd = R"(
float vfc(uint w, uint off, uint bw, uint fl) {
  uint mask = bw == 32u ? 0xFFFFFFFFu : ((1u << bw) - 1u);
  float v = (fl & 1u) ? float((int)(w << (32u - off - bw)) >> (32u - bw)) : float((w >> off) & mask);
  [branch] if ((fl & 2u) == 0u) {
    float umax = bw == 32u ? 4294967295.0 : float(mask);
    float smax = bw == 32u ? 2147483647.0 : float((1u << (bw - 1u)) - 1u);
    if (fl & 1u) v = (fl & 4u) ? (2.0 * v + 1.0) / umax : max(v / smax, -1.0);
    else v = v / umax;
  }
  return v;
}
float4 vfd(uint va, uint fmt, uint fl) {
  uint w0 = VB.Load(va), w1 = 0, w2 = 0, w3 = 0; float4 fv = float4(0, 0, 0, 1);
  [forcecase] switch (fmt) {
    case 6: fv = float4(vfc(w0, 0, 8, fl), vfc(w0, 8, 8, fl), vfc(w0, 16, 8, fl), vfc(w0, 24, 8, fl)); break;
    case 7: fv = float4(vfc(w0, 0, 10, fl), vfc(w0, 10, 10, fl), vfc(w0, 20, 10, fl), vfc(w0, 30, 2, fl)); break;
    case 16: fv.xyz = float3(vfc(w0, 0, 11, fl), vfc(w0, 11, 11, fl), vfc(w0, 22, 10, fl)); break;
    case 17: fv.xyz = float3(vfc(w0, 0, 10, fl), vfc(w0, 10, 11, fl), vfc(w0, 21, 11, fl)); break;
    case 25: fv.xy = float2(vfc(w0, 0, 16, fl), vfc(w0, 16, 16, fl)); break;
    case 26: w1 = VB.Load(va + 4); fv = float4(vfc(w0, 0, 16, fl), vfc(w0, 16, 16, fl), vfc(w1, 0, 16, fl), vfc(w1, 16, 16, fl)); break;
    case 31: fv.xy = float2(f16tof32(w0 & 0xFFFFu), f16tof32((w0 >> 16) & 0xFFFFu)); break;
    case 32: w1 = VB.Load(va + 4); fv = float4(f16tof32(w0 & 0xFFFFu), f16tof32((w0 >> 16) & 0xFFFFu), f16tof32(w1 & 0xFFFFu), f16tof32((w1 >> 16) & 0xFFFFu)); break;
    case 33: fv.x = vfc(w0, 0, 32, fl); break;
    case 34: w1 = VB.Load(va + 4); fv.xy = float2(vfc(w0, 0, 32, fl), vfc(w1, 0, 32, fl)); break;
    case 35: w1 = VB.Load(va + 4); w2 = VB.Load(va + 8); w3 = VB.Load(va + 12); fv = float4(vfc(w0, 0, 32, fl), vfc(w1, 0, 32, fl), vfc(w2, 0, 32, fl), vfc(w3, 0, 32, fl)); break;
    case 36: fv.x = asfloat(w0); break;
    case 37: w1 = VB.Load(va + 4); fv.xy = float2(asfloat(w0), asfloat(w1)); break;
    case 57: w1 = VB.Load(va + 4); w2 = VB.Load(va + 8); fv.xyz = float3(asfloat(w0), asfloat(w1), asfloat(w2)); break;
    case 38: w1 = VB.Load(va + 4); w2 = VB.Load(va + 8); w3 = VB.Load(va + 12); fv = float4(asfloat(w0), asfloat(w1), asfloat(w2), asfloat(w3)); break;
  }
  return fv;
}
)";

// BCR2_VRR: for a G-Sync or FreeSync display. The game clock runs at exactly 60 Hz and each frame is presented at a fixed time after its tick,
// with no wait for a refresh, so the panel follows the game. On a display without variable refresh this tears, so it is opt-in.
static bool g_tearing = false; static UINT g_swapFlags = 0;
static std::atomic<long long> g_lastTickQpc{0};
void gpu_note_tick() { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_lastTickQpc = q.QuadPart; }
static bool tearing_supported() {
    typedef HRESULT (WINAPI *PFN)(REFIID, void**);
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    PFN create = m ? (PFN)GetProcAddress(m, "CreateDXGIFactory1") : nullptr;
    ComPtr<IDXGIFactory5> f; BOOL ok = FALSE;
    if (!create || FAILED(create(__uuidof(IDXGIFactory5), (void**)f.GetAddressOf()))) return false;
    return SUCCEEDED(f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &ok, sizeof ok)) && ok;
}

// Wait until a QPC time. Sleep can wake many milliseconds late, which is a whole missed frame here, so use a high-resolution timer for most of the wait and spin the end.
void gpu_wait_until(long long qpc) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static const double kToMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / (double)f.QuadPart; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double left = (double)(qpc - now.QuadPart) * kToMs;
    if (left > 1.5 && timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 1.0) * 10000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
    }
    do { YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < qpc);
}
// How late the game tick fired against its exact time, reported on the [vrr] line.
std::atomic<int> g_tickLateMaxUs{0};
void gpu_note_tick_late(int us) { int m = g_tickLateMaxUs.load(); while (us > m && !g_tickLateMaxUs.compare_exchange_weak(m, us)) {} }

bool gpu_vrr() { static const bool on = getenv("ASURA_VRR") != nullptr && tearing_supported(); return on; }   // no tearing support: keep the display lock

// ASURA_PULLDOWN=1 (24 fps mode, Frame Rate "24 fps (2:3 pulldown)"): show the game's 24 frames a second on a fixed-refresh display the
// way DVD players show film on a 60 Hz TV. Each frame is presented with vsync for a whole number of refreshes, chosen so the total keeps
// pace with 24 fps: at 60 Hz 3, 2, 3, 2 (2:3 pulldown, 2.5 refreshes a frame on average), at 72 Hz 3 each, at 48 or 96 Hz 2 or 4 each
// (even: no judder), at 50 Hz 2 with a 3 every twelfth frame. D3D allows 1 to 4 refreshes per Present, so displays over 100 Hz
// (120, 144) can't be paced this way: they fall back to one refresh per frame with vsync, and the log says to pick 60 Hz.
// On a 59.94 Hz display 2:3 pulldown shows 23.976 frames a second, so the game's vblank is slowed to match (gpu_pulldown_vblank_hz,
// read by gpu.cpp's vblank thread); otherwise one frame in 1001 would pile up (a hitch every 42 s).
// Returns the refreshes for the frame about to be presented, or 0 when the mode is off. The display's rate is read again every
// 300 frames (the window can move to another monitor).
static double display_hz() {
    ComPtr<IDXGIOutput> out;
    if (!g_swap || FAILED(g_swap->GetContainingOutput(&out))) return 0;
    DXGI_OUTPUT_DESC od{}; out->GetDesc(&od);
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency < 20) return 0;
    const int f = (int)dm.dmDisplayFrequency;
    return (f == 59 || f == 119 || f == 143 || f == 23 || f == 29 || f == 47 || f == 71 || f == 95) ? (f + 1) * 1000.0 / 1001.0 : (double)f;   // NTSC rates
}
static std::atomic<double> g_pulldownVblank{0};                            // the game's vblank rate to match the display (0: as set)
double gpu_pulldown_vblank_hz() { return g_pulldownVblank.load(); }
static UINT pulldown_interval() {
    static const bool kOn = getenv("ASURA_PULLDOWN") && atoi(getenv("ASURA_PULLDOWN")) != 0;
    if (!kOn) return 0;
    static double hz = 0; static uint64_t at = ~0ull, shown = 0;
    static long double acc = 0;                                            // refreshes owed so far, at hz / 24 a frame
    static bool warned = false;
    if (at == ~0ull || g_frame >= at + 300) {
        at = g_frame;
        const double h = display_hz();
        if (h > 0 && std::fabs(h - hz) > 0.01) { hz = h; acc = 0; shown = 0; g_pulldownVblank = 48.0 * hz / std::round(hz); fprintf(stderr, "[pulldown] display at %.3f Hz: %.3f refreshes a frame\n", hz, hz / 24.0); warned = false; }
    }
    if (hz <= 0) return 1;
    const double per = std::round(hz) / 24.0;                               // the nominal cadence (2.5 at 59.94 or 60 Hz): exact 3, 2, 3, 2; the game's
                                                                            // vblank is slowed to hz / per instead (g_pulldownVblank)
    if (per > 4.2 || per < 0.9) {
        if (!warned) { warned = true; fprintf(stderr, "[pulldown] %.0f Hz can't be paced at 24 fps (1 to 4 refreshes a frame): one refresh per frame; set the display to 60 Hz\n", hz); }
        return 1;
    }
    acc += per;
    long long n = (long long)std::floor(acc + 0.5) - (long long)shown;    // the frame's whole refreshes: 3, 2, 3, 2 at 60 Hz
    n = std::clamp<long long>(n, 1, 4);
    shown += (uint64_t)n;
    if (acc > 1e9) { acc -= (long double)shown; shown = 0; }
    return (UINT)n;
}

// Laptop drivers (NVIDIA Optimus, AMD switchable graphics) run an exe on the discrete GPU when it exports these.
extern "C" { __declspec(dllexport) DWORD NvOptimusEnablement = 1; __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1; }

static bool device_init() {
    if (g_trace == 0) if (const char* t = getenv("ASURA_GPUTRACE")) g_trace = atoi(t);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, window_thread, ev, 0, nullptr);
    WaitForSingleObject(ev, 10000);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = 1280; sd.BufferDesc.Height = 720; sd.BufferDesc.Format = g_backFmt;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (gpu_vrr()) { sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; g_swapFlags = sd.Flags; g_tearing = true; }
    fprintf(stderr, "[vrr] %s, present with tearing allowed: %s\n", gpu_vrr() ? "variable refresh pacing" : "fixed refresh pacing", g_tearing ? "yes" : "no");
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got;
    // The GPU: the high-performance one (DXGI's preference order), not whatever is adapter 0. On a machine with integrated graphics
    // as well as a discrete card, the default adapter can be the integrated one. ASURA_ADAPTER=<n> picks the n-th in that order.
    ComPtr<IDXGIAdapter1> adapter;
    {
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f6)))) {
            const UINT pick = getenv("ASURA_ADAPTER") ? (UINT)atoi(getenv("ASURA_ADAPTER")) : 0;
            for (UINT i = 0;; i++) {
                ComPtr<IDXGIAdapter1> a;
                if (FAILED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
                DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
                char name[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
                const bool sw = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                fprintf(stderr, "[gfx] adapter %u: %s, %llu MB video memory%s%s\n", i, name, (unsigned long long)(ad.DedicatedVideoMemory >> 20), sw ? " (software)" : "", i == pick ? " <- used" : "");
                if (i == pick && !sw) adapter = a;
            }
        }
    }
    const D3D_DRIVER_TYPE dt = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;   // an explicit adapter needs UNKNOWN
    HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter.Get(), dt, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(adapter.Get(), dt, nullptr, 0, want + 1, 1, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr) && adapter) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { fprintf(stderr, "[gfx] D3D11 device creation failed: %08lX; running without rendering\n", hr); return false; }
    g_featureLevel = got;
    g_ctx.As(&g_ctx1);
    {   // Queue at most one finished frame ahead of the display. The default of three adds visible input lag.
        ComPtr<IDXGIDevice1> dxgi;
        if (SUCCEEDED(g_dev.As(&dxgi))) dxgi->SetMaximumFrameLatency(1);
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
    if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt)))) g_noOverwriteSrv = opt.MapNoOverwriteOnDynamicBufferSRV && !getenv("ASURA_NONOOVERWRITE");
    g_swap->GetBuffer(0, IID_PPV_ARGS(&g_backTex));
    g_dev->CreateRenderTargetView(g_backTex.Get(), nullptr, &g_backRtv);
    {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory)))) factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (const char* mode = getenv("ASURA_DISPLAY")) {
            if (!strcmp(mode, "borderless")) g_modeWanted = kBorderless;
            else if (!strcmp(mode, "fullscreen")) g_modeWanted = kBorderless;   // exclusive fullscreen is gone: the old name means borderless
        }
        D3D11_SAMPLER_DESC sd2{}; sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd2.MaxLOD = D3D11_FLOAT32_MAX;
        g_dev->CreateSamplerState(&sd2, &g_presentSampler);
    }

    auto mkcb = [](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = size; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&d, nullptr, &out);
    };
    mkcb(8192, g_cbFloat); mkcb(4096, g_cbState); mkcb(48, g_cbBlit);
    D3D11_BUFFER_DESC vb{}; vb.ByteWidth = kVbRingSize; vb.BindFlags = D3D11_BIND_SHADER_RESOURCE; vb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (g_noOverwriteSrv) { vb.Usage = D3D11_USAGE_DYNAMIC; vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
    else vb.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sv.BufferEx.NumElements = kVbRingSize / 4; sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    {   // Several rings, so a wrap can move to one the GPU has finished with. Discarding a ring this large stalled the render thread for 3 to 15 ms.
        D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
        for (int i = 0; i < kVbRings; i++) {
            g_dev->CreateBuffer(&vb, nullptr, &g_vbRings[i]);
            g_dev->CreateShaderResourceView(g_vbRings[i].Get(), &sv, &g_vbSrvs[i]);
            g_dev->CreateQuery(&qd, &g_vbDone[i]);
        }
        g_vbRing = g_vbRings[0]; g_vbSrv = g_vbSrvs[0];
    }
    D3D11_BUFFER_DESC ib{}; ib.ByteWidth = kIbRingSize; ib.Usage = D3D11_USAGE_DYNAMIC; ib.BindFlags = D3D11_BIND_INDEX_BUFFER; ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&ib, nullptr, &g_ibRing);

    // Rectangle list: emit the fourth corner, opposite the corner between the two shorter edges.
    std::string gs = std::string(kVOut) +
        "[maxvertexcount(4)] void main(triangle VOut v[3], inout TriangleStream<VOut> os) {\n"
        "  float2 p0=v[0].pos.xy/v[0].pos.w, p1=v[1].pos.xy/v[1].pos.w, p2=v[2].pos.xy/v[2].pos.w;\n"
        "  float d01=dot(p1-p0,p1-p0), d12=dot(p2-p1,p2-p1), d20=dot(p0-p2,p0-p2);\n"
        "  int c = (d12>=d01 && d12>=d20) ? 0 : ((d20>=d01) ? 1 : 2);\n"
        "  VOut C=v[c], A=v[(c+1)%3], B=v[(c+2)%3], D;\n"
        "  D.pos=A.pos+B.pos-C.pos; for(int i=0;i<16;i++) D.o[i]=A.o[i]+B.o[i]-C.o[i];\n"
        "  os.Append(A); os.Append(C); os.Append(B); os.Append(D); }\n";
    if (auto b = compile_cached(gs, "main", "gs_5_0")) g_dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_gsRect);
    const char* blitVs = "float4 main(uint i : SV_VertexID) : SV_Position { float2 t=float2((i<<1)&2,i&2); return float4(t.x*2-1,1-t.y*2,0,1); }\n";
    if (auto b = compile_cached(blitVs, "main", "vs_5_0")) g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vsBlit);
    const char* blitPs =
        "Texture2D src:register(t0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; float gam; float pv; float aa; float es; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float4 c=src.Load(int3(int2(p.xy*scale+ofs),0)); if(es!=0.0) c*=es; return swaprb? c.bgra : c; }\n";
    if (auto b = compile_cached(blitPs, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlit);
    const char* blitDs =
        "Texture2D src:register(t0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { float d=src.Load(int3(int2(p.xy*scale+ofs),0)).r; return float4(d,d,d,1); }\n";
    if (auto b = compile_cached(blitDs, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlitDepth);
    const char* blitRaw =
        "Texture2D<float> src:register(t0); Texture2D<uint2> sten:register(t1); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; };\n"
        "float4 main(float4 p:SV_Position):SV_Target { int3 c=int3(int2(p.xy*scale+ofs),0); float x=src.Load(c); uint d=(x>=0.5)?((uint)round(x*16777216.0)-1u):(uint)round(x*16777215.0); d=min(d,0xFFFFFFu); uint s=sten.Load(c).g;\n"
        "  uint w=(d<<8)|(s&255); return float4(w&255,(w>>8)&255,(w>>16)&255,(w>>24)&255)/255.0; }\n";
    if (auto b = compile_cached(blitRaw, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psBlitRaw);
    // Present samples with a filter, so the picture can be scaled. At 1:1 every sample lands on a texel centre, which gives the texel unchanged.
    // The final pass. Several samples per output pixel when the picture is bigger than the window (supersampling), the game's own gamma ramp, the player's gamma,
    // and the gamma screen's sample picture under the menu (t2) while it is open.
    const char* presentCommon =
        "Texture2D src:register(t0); Texture2D lut:register(t1); Texture2D prv:register(t2); SamplerState smp:register(s0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; float gam; float pv; float aa; };\n"
        "static float2 dimv;\n"
        "float4 tap(float2 t){ float4 c=src.SampleLevel(smp,t/dimv,0); return swaprb? c.bgra : c; }\n"
        "float luma(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }\n"
        // FXAA (after Timothy Lottes' console version): find the local edge direction from the luma of the four corners and blend along it. aa 1 reaches 4 texels, 2 reaches 8.
        "float4 shade(float2 t){ float4 m=tap(t); if(aa<0.5) return m;\n"
        "  float3 nw=tap(t+float2(-1,-1)).rgb, ne=tap(t+float2(1,-1)).rgb, sw=tap(t+float2(-1,1)).rgb, se=tap(t+float2(1,1)).rgb;\n"
        "  float lM=luma(m.rgb), lNW=luma(nw), lNE=luma(ne), lSW=luma(sw), lSE=luma(se);\n"
        "  float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))), lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));\n"
        "  if(lMax-lMin < max(0.0312, lMax*0.125)) return m;\n"
        "  float2 dir=float2(-((lNW+lNE)-(lSW+lSE)), ((lNW+lSW)-(lNE+lSE)));\n"
        "  float reduce=max((lNW+lNE+lSW+lSE)*0.03125, 0.0078125);\n"
        "  float rcp=1.0/(min(abs(dir.x),abs(dir.y))+reduce); float span=(aa>1.5)?8.0:4.0;\n"
        "  dir=clamp(dir*rcp,-span,span);\n"
        "  float3 A=0.5*(tap(t+dir*(1.0/3.0-0.5)).rgb+tap(t+dir*(2.0/3.0-0.5)).rgb);\n"
        "  float3 B=A*0.5+0.25*(tap(t+dir*-0.5).rgb+tap(t+dir*0.5).rgb);\n"
        "  float lB=luma(B); return float4((lB<lMin||lB>lMax)? A : B, 1.0); }\n"
        "float4 main(float4 p:SV_Position):SV_Target { float w,h; src.GetDimensions(w,h); dimv=float2(w,h); float2 c0=p.xy*scale+ofs; float4 c;\n"
        "  int2 n=(int2)clamp(ceil(scale-0.001),1.0,4.0);\n"
        "  if(n.x>1||n.y>1){ float4 a=0; for(int j=0;j<n.y;j++) for(int i=0;i<n.x;i++){ float2 o=((float2(i,j)+0.5)/n-0.5)*scale; a+=shade(c0+o); } c=a/(float)(n.x*n.y); }\n"
        "  else c=shade(c0);\n"
        "  float3 col=c.rgb;\n"
        "#ifdef LUT\n"
        "  float3 u=(saturate(col)*255.0+0.5)/256.0;\n"
        "  col=float3(lut.SampleLevel(smp,float2(u.r,0.5),0).r, lut.SampleLevel(smp,float2(u.g,0.5),0).g, lut.SampleLevel(smp,float2(u.b,0.5),0).b);\n"
        "#endif\n"
        "  col=saturate(col);\n"
        "  if(pv>0.5){ float3 im=prv.SampleLevel(smp,c0/dimv,0).rgb; col=saturate(col+im*(1.0-max(col.r,max(col.g,col.b)))); }\n"
        "  if(abs(gam-1.0)>0.001) col=pow(col,1.0/gam);\n"
        "  return float4(col,1.0); }\n";    if (auto b = compile_cached(std::string("#define LUT 1\n") + presentCommon, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPresentLut);
    if (auto b = compile_cached(presentCommon, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPresentPlain);    {
        D3D11_TEXTURE2D_DESC d{}; d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R10G10B10A2_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &g_lutTex);
        if (g_lutTex) g_dev->CreateShaderResourceView(g_lutTex.Get(), nullptr, &g_lutSrv);
    }
    // EDRAM aliasing: as 8_8_8_8 the sample is A<<24|B<<16|G<<8|R, as D24S8 the same dword is D24<<8|S8. Depth tiles
    // also have their 40-pixel left and right halves swapped relative to colour (the game's restore shader assumes this).
    const char* c2d =
        "Texture2D src:register(t0);\n"
        "float main(float4 p:SV_Position):SV_Depth { int x=(int)p.x; x+=((x%(80*RS))<40*RS)?40*RS:-40*RS; float4 c=src.Load(int3(x,(int)p.y,0));\n"
        "  uint a=(uint)(c.a*255.0+0.5), b=(uint)(c.b*255.0+0.5), g=(uint)(c.g*255.0+0.5);\n"
        // Lossless: the game redraws meshes with depth EQUAL, so this must round to exactly the same D24 value.
        // Above 0.5 a float can only hold multiples of 2^-24, and (d+1)*2^-24 is the one that rounds back to d.
        "  uint d=(a<<16)|(b<<8)|g; return (d>=0x800000u)?(float)(d+1u)*5.9604644775390625e-8:(float)d/16777215.0; }\n";
    if (auto b = compile_cached(S("#define RS %d\n", g_resScale) + c2d, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColorToDepth);
    fprintf(stderr, "[gfx] D3D11 ready, feature level %X, noOverwriteSrv=%d\n", (unsigned)got, (int)g_noOverwriteSrv);
    return true;
}

// ---- shader translation ----
struct TexUse { uint32_t slot, dim; int magf = -1, minf = -1, mipf = -1; };
struct VfUse { uint32_t slot, stride; };
struct XShader {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    std::vector<TexUse> tex;
    std::vector<VfUse> vf;
    bool ok = false;
    bool memexport = false;                                                  // vertex shader writes memory (eA/eM exports): drawn through issue_memexport
    uint64_t hash = 0;
    std::atomic<bool> ready{false};                                          // false while a worker is compiling it; ok, vs and ps are valid once true
    // Generic vertex shaders (see vs_variant): a base is compiled from the unpatched code in the game files and records where its vertex
    // fetches and interpolator exports are; a variant is one patched copy the game binds, sharing its base's compiled shader.
    bool dynBase = false;
    std::vector<uint32_t> fetchPos, expPos;
    uint64_t fetchDstMask = 0;                                                  // generic VS: registers its fetches may write (each fetch's own)
    XShader* base = nullptr;
    uint32_t vsf[32 * 4] = {};
    int32_t vsx[64] = {};
};
// The shader that actually runs: a generic vertex shader's variant runs its base.
static inline XShader* run_sh(XShader* s) { return s && s->base ? s->base : s; }

// New shaders compile on worker threads, so a mesh seen for the first time costs a frame or two of that mesh, not a freeze of the whole game.
struct CompileJob { XShader* sh; std::string src; bool isVs; std::string fallback; };   // fallback: compiled if src is refused
static std::mutex g_compileLock;
static std::condition_variable g_compileWake;
static std::deque<CompileJob> g_compileQueue;
static std::atomic<int> g_compilePending{0};                                 // queued or being compiled
static void finish_shader(XShader* sh, ID3DBlob* blob, bool isVs) {
    if (blob) {
        if (isVs) sh->ok = SUCCEEDED(g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sh->vs));
        else sh->ok = SUCCEEDED(g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sh->ps));
    }
    sh->ready.store(true, std::memory_order_release);
}
static void compile_worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        CompileJob job;
        {
            std::unique_lock<std::mutex> lk(g_compileLock);
            g_compileWake.wait(lk, [] { return !g_compileQueue.empty(); });
            job = std::move(g_compileQueue.front()); g_compileQueue.pop_front();
        }
        auto blob = compile_cached(job.src, "main", job.isVs ? "vs_5_0" : "ps_5_0");
        if (!blob && !job.fallback.empty()) {
            fprintf(stderr, "[gfx] shader %016llX: using the level-0 sampling copy\n", (unsigned long long)job.sh->hash);
            blob = compile_cached(job.fallback, "main", job.isVs ? "vs_5_0" : "ps_5_0");
        }
        finish_shader(job.sh, blob.Get(), job.isVs);
        g_compilePending--;
    }
}
// The same shader with every gradient sample (Sample, SampleBias) turned into SampleLevel(..., 0.0): what loop shaders always used before.
static std::string to_level0(const std::string& src) {
    std::string out; out.reserve(src.size() + 256);
    size_t pos = 0;
    for (;;) {
        size_t a = src.find(".Sample(", pos), b = src.find(".SampleBias(", pos);
        size_t at = std::min(a, b);
        if (at == std::string::npos) { out.append(src, pos, std::string::npos); break; }
        const size_t open = src.find('(', at);
        out.append(src, pos, at - pos);
        std::vector<std::string> args; std::string cur; int depth = 0; size_t i = open + 1;
        for (; i < src.size(); i++) {
            const char ch = src[i];
            if (ch == '(') depth++;
            else if (ch == ')') { if (depth == 0) break; depth--; }
            else if (ch == ',' && depth == 0) { args.push_back(cur); cur.clear(); continue; }
            cur += ch;
        }
        args.push_back(cur);
        out += ".SampleLevel(" + args[0] + "," + args[1] + ",0.0)";
        pos = i + 1;
    }
    return out;
}
static void queue_compile(XShader* sh, std::string src, bool isVs, std::string fallback = {}) {
    static std::once_flag once;
    std::call_once(once, [] {
        const unsigned n = std::max(1u, std::min(6u, std::thread::hardware_concurrency() / 3));
        for (unsigned i = 0; i < n; i++) std::thread(compile_worker).detach();
        fprintf(stderr, "[gfx] %u background shader compile threads\n", n);
    });
    g_compilePending++;
    { std::lock_guard<std::mutex> lk(g_compileLock); g_compileQueue.push_back({ sh, std::move(src), isVs, std::move(fallback) }); }
    g_compileWake.notify_one();
}
// Whether a draw can go ahead. Full-screen passes wait, since leaving one out would flash the whole screen. So does everything during a large batch, as
// when a level first appears: half a scene missing for most of a second looks worse than a short wait. Otherwise a draw waits only for what is left of a
// small per-frame budget and is then left out for this frame.
static double g_compileWaitMs = 0;                                           // used this frame
static uint64_t g_drawsLeftOut = 0, g_leftOutVs = 0, g_leftOutPs = 0;   // draws left out, and how many waited on their vertex / pixel shader
static bool shaders_ready(XShader* a, XShader* b, bool mustWait) {
    a = run_sh(a); b = run_sh(b);
    auto done = [&] { return (!a || a->ready.load(std::memory_order_acquire)) && (!b || b->ready.load(std::memory_order_acquire)); };
    if (done()) return true;
    static const double kBudgetMs = getenv("ASURA_COMPILEWAIT") ? atof(getenv("ASURA_COMPILEWAIT")) : 5.0;
    static const int kBatch = getenv("ASURA_COMPILEBATCH") ? atoi(getenv("ASURA_COMPILEBATCH")) : 16;
    if (g_compilePending.load() >= kBatch) mustWait = true;
    LARGE_INTEGER s, q; QueryPerformanceCounter(&s);
    for (;;) {
        SwitchToThread();
        if (done()) break;
        QueryPerformanceCounter(&q);
        if (!mustWait && g_compileWaitMs + (double)(q.QuadPart - s.QuadPart) * g_perfTicksToMs >= kBudgetMs) break;
    }
    QueryPerformanceCounter(&q);
    g_compileWaitMs += (double)(q.QuadPart - s.QuadPart) * g_perfTicksToMs;
    if (done()) return true;
    g_drawsLeftOut++;
    if (a && !a->ready.load(std::memory_order_acquire)) g_leftOutVs++;
    if (b && !b->ready.load(std::memory_order_acquire)) g_leftOutPs++;
    return false;
}

// Declarations shared by both stages.
static const char* kCommon = R"(
cbuffer CF : register(b0) { float4 c[512]; };
cbuffer CS : register(b1) {
  uint4 bools[2]; uint4 loops[8];
  float4 vpNdc;     // x scale, y scale, x offset, y offset
  float4 vpZ;       // z scale, z offset, xy_fmt, z_fmt
  float4 vpW;       // w0_fmt, index offset
  float4 alpha;     // func, ref
  float4 vpSnap;    // target width and height (0 = off), rest of the half pixel, half host pixel already applied
  uint4 vfo[24];
  int4 tfsw[32];
  float4 tfp[32];   // uv scale x/y, exp scale, cube/unused
  uint4 tfsg[32];
  float4 tfi[32];   // 1/width, 1/height of the bound host texture
  uint4 vsf[32];    // generic vertex shaders: per vertex fetch x = slot | stride<<8 | format<<16 | flags<<24 (1 signed, 2 int, 4 no-zero, 8 round),
                    //   y = offset (dwords, signed), z = dest swizzle | exp adjust<<12 | dest register<<18
  int4 vsx[16];     // generic vertex shaders: per export instruction, the interpolator it writes (-1: blanked by the game)
};
Texture2D T2[16] : register(t0);
TextureCube TC[16] : register(t16);
SamplerState SS[16] : register(s0);
ByteAddressBuffer VB : register(t64);
// Xenos multiplies as Direct3D 9 did: 0 times anything (infinity, NaN) is 0, in MUL, MAD, the dot products and the scalar multiplies (as Xenia).
// Plain IEEE products turned 1/sqrt(0) and log2(0) into NaN where the console gets 0: specks on bumpy, lit metal.
// Bilinear filtering on a fetch's own grid (size 1/inv texels) in a render-scale surface: the four nearest texel centres, blended with the
// console's weights. uv is in surface space (fetch space times sc); yoff moves to a slice of a stacked fetch.
float4 gridlerp(Texture2D t, SamplerState s, float2 uv, float2 sc, float2 inv, float yoff) {
  float2 sz = 1.0 / inv, p = uv / sc * sz - 0.5, i0 = floor(p), f = p - i0;
  float2 a = clamp(i0, 0.0, sz - 1.0), b = clamp(i0 + 1.0, 0.0, sz - 1.0);
  float2 ua = (a + 0.5) * inv * sc, ub = (b + 0.5) * inv * sc;
  float4 c00 = t.SampleLevel(s, float2(ua.x, ua.y + yoff), 0), c10 = t.SampleLevel(s, float2(ub.x, ua.y + yoff), 0);
  float4 c01 = t.SampleLevel(s, float2(ua.x, ub.y + yoff), 0), c11 = t.SampleLevel(s, float2(ub.x, ub.y + yoff), 0);
  return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}
float4 m0(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float m0(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 texfix(float4 t, int n) {
  uint4 g = tfsg[n];
  float4 b = t * 2.0 - 1.0;
  float4 s = (t >= 0.5) ? (t * 255.0 - 256.0) / 127.0 : t * 255.0 / 127.0;
  // 16-bit data (tfi.w): 1 = held as unorm16, signed is two's complement on 16 bits; 2 = held as snorm16 (a resolve to snorm),
  // unsigned reads those bits as unorm; 3 = float (16-bit float formats and _EXPAND), the sign setting doesn't apply
  if (tfi[n].w > 2.5) { s = t; b = t; }
  else if (tfi[n].w > 1.5) { s = t; t = (t >= 0.0) ? t * (32767.0 / 65535.0) : 1.0 + t * (32767.0 / 65535.0); }
  else if (tfi[n].w > 0.5) {
    float4 i = floor(t * 65535.0 + 0.5), si = i - ((i >= 32768.0) ? 65536.0 : 0.0);
    s = max(si / 32767.0, -1.0);
  }
  // Xenos piecewise-linear gamma to linear
  float4 lin = (t < 0.25) ? t * 0.25 : ((t < 0.375) ? t * 0.5 - 0.0625 : ((t < 0.75) ? t - 0.25 : t * 2.0 - 1.0));
  t = (g == 1) ? s : ((g == 2) ? b : ((g == 3) ? lin : t));
  t *= tfp[n].z;
  float v6[6] = { t.x, t.y, t.z, t.w, 0.0, 1.0 };
  int4 w = min(tfsw[n], 5);
  return float4(v6[w.x], v6[w.y], v6[w.z], v6[w.w]);
}
float3 cubedir(float3 u) {
  float sc = (u.x - 1.5) * 2.0, tc = (u.y - 1.5) * 2.0;
  int f = (int)(u.z + 0.5);
  if (f == 0) return float3(1, -tc, -sc);
  if (f == 1) return float3(-1, -tc, sc);
  if (f == 2) return float3(sc, 1, tc);
  if (f == 3) return float3(sc, -1, -tc);
  if (f == 4) return float3(sc, -tc, 1);
  return float3(-sc, -tc, -1);
}
)";

// Memexport (vertex shaders that write memory through eA/eM, as Xenia decodes them). UE3 moves texture memory this way (its texture pool
// defragmenter and mip copies). Each write goes to MX as a record (byte address, value, byte mask); issue_draw reads them back after the draw
// and applies them to guest memory. MX dword 0 counts the records; records start at byte 16.
// eA: x = 0x40000000 | base address in dwords, y = 2^23 + element index, z = format word (bits 0-2 endian, 8-13 colour format, 16 signed,
// 17 integer, 19 red/blue swap, 20-31 0x4B0), w = 2^23 + element count. eM0..eM4 go to elements index+0..index+4.
static const uint32_t kMxCap = 1u << 21;                                     // records per draw
static const char* kMemExport = R"(
RWByteAddressBuffer MX : register(u1);
void mx_put(uint addr, uint val, uint mask) {
  uint slot; MX.InterlockedAdd(0, 1u, slot);
  if (slot < 2097152u) MX.Store3(16u + slot * 16u, uint3(addr, val, mask));
}
uint mx_sw(uint v, uint e) {
  if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u || e == 4u || e == 5u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (e == 3u) return (v << 16) | (v >> 16);
  return v;
}
uint mx_n(float f, uint w, uint sgn, uint integ) {
  if (isnan(f)) f = 0.0;
  uint m = (1u << w) - 1u;
  if (integ != 0u) {
    if (sgn != 0u) { float h = (float)(1u << (w - 1u)); float g = clamp(f, -h, h - 1.0); return (uint)(int)(g + (g < 0.0 ? -0.5 : 0.5)) & m; }
    return (uint)(clamp(f, 0.0, (float)m) + 0.5) & m;
  }
  if (sgn != 0u) { float g = clamp(f, -1.0, 1.0) * (float)((1u << (w - 1u)) - 1u); return (uint)(int)(g + (g < 0.0 ? -0.5 : 0.5)) & m; }
  return (uint)(saturate(f) * (float)m + 0.5) & m;
}
void mx_flush(float4 eA, float4 eM[5], uint emw) {
  uint4 a = asuint(eA);
  if ((a.x >> 30) != 1u || (a.y >> 23) != 0x96u || (a.z >> 23) != 0x96u || (a.w >> 23) != 0x96u) return;
  uint idx = a.y & 0x7FFFFFu, cnt = a.w & 0x7FFFFFu, base = (a.x & 0x3FFFFFFFu) << 2;
  uint e = a.z & 7u, fmt = (a.z >> 8) & 63u, sg = (a.z >> 16) & 1u, it = (a.z >> 17) & 1u, rbs = (a.z >> 19) & 1u;
  uint lb = 9u;
  if (fmt == 2u || fmt == 8u || fmt == 9u) lb = 0u;
  else if (fmt == 4u || fmt == 10u || fmt == 24u || fmt == 30u) lb = 1u;
  else if (fmt == 6u || fmt == 14u || fmt == 50u || fmt == 7u || fmt == 54u || fmt == 25u || fmt == 31u || fmt == 36u) lb = 2u;
  else if (fmt == 26u || fmt == 32u || fmt == 37u) lb = 3u;
  else if (fmt == 38u) lb = 4u;
  if (lb == 9u) { if (emw != 0u) mx_put(0xFFFFFFFFu, fmt, a.z); return; }
  [unroll] for (uint n = 0u; n < 5u; n++) {
    if (((emw >> n) & 1u) == 0u || idx + n >= cnt) continue;
    float4 v = eM[n]; if (rbs != 0u) v = v.zyxw;
    uint d[4] = { 0u, 0u, 0u, 0u };
    if (fmt == 38u) { d[0] = asuint(v.x); d[1] = asuint(v.y); d[2] = asuint(v.z); d[3] = asuint(v.w); }
    else if (fmt == 37u) { d[0] = asuint(v.x); d[1] = asuint(v.y); }
    else if (fmt == 36u) d[0] = asuint(v.x);
    else if (fmt == 6u || fmt == 14u || fmt == 50u) d[0] = mx_n(v.x, 8u, sg, it) | (mx_n(v.y, 8u, sg, it) << 8) | (mx_n(v.z, 8u, sg, it) << 16) | (mx_n(v.w, 8u, sg, it) << 24);
    else if (fmt == 7u || fmt == 54u) d[0] = mx_n(v.x, 10u, sg, it) | (mx_n(v.y, 10u, sg, it) << 10) | (mx_n(v.z, 10u, sg, it) << 20) | (mx_n(v.w, 2u, sg, it) << 30);
    else if (fmt == 26u) { d[0] = mx_n(v.x, 16u, sg, it) | (mx_n(v.y, 16u, sg, it) << 16); d[1] = mx_n(v.z, 16u, sg, it) | (mx_n(v.w, 16u, sg, it) << 16); }
    else if (fmt == 25u) d[0] = mx_n(v.x, 16u, sg, it) | (mx_n(v.y, 16u, sg, it) << 16);
    else if (fmt == 24u) d[0] = mx_n(v.x, 16u, sg, it);
    else if (fmt == 32u) { d[0] = f32tof16(v.x) | (f32tof16(v.y) << 16); d[1] = f32tof16(v.z) | (f32tof16(v.w) << 16); }
    else if (fmt == 31u) d[0] = f32tof16(v.x) | (f32tof16(v.y) << 16);
    else if (fmt == 30u) d[0] = f32tof16(v.x);
    else if (fmt == 10u) d[0] = mx_n(v.x, 8u, sg, it) | (mx_n(v.y, 8u, sg, it) << 8);
    else if (fmt == 4u) d[0] = mx_n(v.x, 5u, sg, it) | (mx_n(v.y, 6u, sg, it) << 5) | (mx_n(v.z, 5u, sg, it) << 11);
    else d[0] = mx_n(v.x, 8u, sg, it);
    uint addr = base + ((idx + n) << lb);
    if (lb >= 2u) {
      uint nd = lb == 2u ? 1u : (lb == 3u ? 2u : 4u);
      if (e == 4u && nd >= 2u) { uint t = d[0]; d[0] = d[1]; d[1] = t; t = d[2]; d[2] = d[3]; d[3] = t; }
      if (e == 5u && nd == 4u) { uint t = d[0]; d[0] = d[3]; d[3] = t; t = d[1]; d[1] = d[2]; d[2] = t; }
      [unroll] for (uint k = 0u; k < 4u; k++) if (k < nd) mx_put(addr + 4u * k, mx_sw(d[k], e), 0xFFFFFFFFu);
    } else {
      uint sh = (addr & 3u) * 8u, m = (lb == 0u ? 0xFFu : 0xFFFFu);
      mx_put(addr & ~3u, (mx_sw(d[0], e) & m) << sh, m << sh);
    }
  }
}
)";
static const char* kMxFlush = "{ if (emw != 0u) { mx_flush(eA, eM, emw); emw = 0u; } }";   // what each MX_FLUSH(); marker becomes

struct Translator {
    const uint32_t* ucode; uint32_t words; bool isVs;
    std::string body;
    std::set<int> usedRegs;
    bool useArray = false, needRel = false, writesDepth = false, hasLoop = false, memexport = false;
    bool exact = false;                                                     // memexport shaders: moves keep raw bits (set on a second walk)
    XShader* out;
    std::map<uint32_t, TexUse> texs; std::map<uint32_t, uint32_t> vfs;
    int uniq = 0;
    uint32_t prevFull[3] = { 0, 0, 0 };
    // Generic vertex shader (dynVs): fetch parameters and export targets come from vsf/vsx at draw time, so one compiled shader serves every
    // patched copy the game makes of it. The word offsets of each vertex fetch and each interpolator export, in emission order.
    bool dynVs = false;
    std::vector<uint32_t> dynFetchPos, dynExpPos;
    uint64_t dynDstMask = 0;                                                     // registers written by the fetches (generic VS)

    std::string R(int n) {
        n &= 63; usedRegs.insert(n);
        return useArray ? S("r[%d]", n) : S("r%d", n);
    }
    std::string Rrel(int n, bool rel) {
        if (!rel) return R(n);
        needRel = true;
        return S("r[clamp(aL+%d,0,63)]", n & 63);
    }
    static char comp(int i) { return "xyzw"[i & 3]; }
    static int swz(uint32_t s, int c) { return ((s >> (2 * c)) + c) & 3; }

    static uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)); }

    std::string src_base(const uint32_t* d, int i) {     // i = 1..3, unswizzled value with abs (no negate)
        uint32_t d1 = d[1], d2 = d[2];
        uint32_t reg = i == 1 ? bits(d2, 16, 8) : i == 2 ? bits(d2, 8, 8) : bits(d2, 0, 8);
        bool isTemp = i == 1 ? bits(d2, 31, 1) : i == 2 ? bits(d2, 30, 1) : bits(d2, 29, 1);
        bool s1t = bits(d2, 31, 1), s2t = bits(d2, 30, 1);
        std::string e;
        if (isTemp) {
            e = Rrel(reg & 63, (reg & 0x40) != 0);
            if (reg & 0x80) e = "abs(" + e + ")";
        } else {
            bool c0 = bits(d1, 31, 1), c1 = bits(d1, 30, 1), arel = bits(d1, 29, 1);
            bool addressed = i == 1 ? c0 : i == 2 ? (s1t ? c0 : c1) : ((s1t && s2t) ? c0 : c1);
            int idx = reg + (isVs ? 0 : 256);
            if (addressed) e = S("c[clamp(%s+%d,0,511)]", arel ? "a0" : "aL", idx);
            else e = S("c[%d]", idx);
            if (bits(d[0], 7, 1)) e = "abs(" + e + ")";
        }
        return e;
    }
    std::string src_vec(const uint32_t* d, int i) {
        uint32_t sw = bits(d[1], i == 1 ? 16 : i == 2 ? 8 : 0, 8);
        bool neg = bits(d[1], i == 1 ? 26 : i == 2 ? 25 : 24, 1);
        std::string e = src_base(d, i);
        if (sw != 0) e += S(".%c%c%c%c", comp(swz(sw, 0)), comp(swz(sw, 1)), comp(swz(sw, 2)), comp(swz(sw, 3)));
        return neg ? "-(" + e + ")" : e;
    }

    void emit_alu(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        uint32_t vdst = bits(d0, 0, 6), sdst = bits(d0, 8, 6), vmask = bits(d0, 16, 4), smask = bits(d0, 20, 4);
        bool vrel = bits(d0, 6, 1), srel = bits(d0, 14, 1), exp = bits(d0, 15, 1), vclamp = bits(d0, 24, 1), sclamp = bits(d0, 25, 1);
        uint32_t sop = bits(d0, 26, 6), vop = bits(d2, 24, 5);
        bool pred = bits(d1, 28, 1), predc = bits(d1, 27, 1);
        std::string& B = body;
        int dynExp = -1;                                                    // generic vertex shader: this export's slot in vsx
        if (dynVs && isVs && exp && vdst < 16) {
            dynExp = (int)dynExpPos.size();
            dynExpPos.push_back((uint32_t)(d - ucode));
            B += S("if (vsx[%d].%c >= 0) ", dynExp >> 2, comp(dynExp & 3));  // blanked by the game: the whole instruction is a no-op
        }
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        // vector result needed?
        uint32_t c0Mask = 0, c1Mask = 0, vRes = vmask, sRes = smask;
        if (exp) {
            vRes = vmask & ~smask; sRes = smask & ~vmask;
            c1Mask = vmask & smask;
            if (srel) c0Mask = 0xF & ~(vmask | smask);
        }
        bool vSide = (vop >= 20 && vop <= 27) || vop == 29;
        if (vRes || vSide) {
            B += " float4 s1=" + src_vec(d, 1) + ", s2=" + src_vec(d, 2) + ", s3=" + src_vec(d, 3) + ", v=0;\n";
            static const bool kIeeeMul = getenv("ASURA_IEEEMUL") != nullptr;  // the old plain products, to compare
            const bool z = !kIeeeMul;
            switch (vop) {
              case 0: B += " v=s1+s2;\n"; break;
              case 1: B += z ? " v=m0(s1,s2);\n" : " v=s1*s2;\n"; break;
              case 2:   // max of a value with itself is the console's mov; memexport copies raw bits that float max would change (denormals)
                B += (exact && src_vec(d, 1) == src_vec(d, 2)) ? " v=s1;\n" : " v=max(s1,s2);\n"; break;
              case 3: B += " v=min(s1,s2);\n"; break;
              case 4: B += " v=(s1==s2)?1.0:0.0;\n"; break;
              case 5: B += " v=(s1>s2)?1.0:0.0;\n"; break;
              case 6: B += " v=(s1>=s2)?1.0:0.0;\n"; break;
              case 7: B += " v=(s1!=s2)?1.0:0.0;\n"; break;
              case 8: B += " v=frac(s1);\n"; break;
              case 9: B += " v=trunc(s1);\n"; break;
              case 10: B += " v=floor(s1);\n"; break;
              case 11: B += z ? " v=m0(s1,s2)+s3;\n" : " v=s1*s2+s3;\n"; break;
              case 12: B += " v=(s1==0.0)?s2:s3;\n"; break;
              case 13: B += " v=(s1>=0.0)?s2:s3;\n"; break;
              case 14: B += " v=(s1>0.0)?s2:s3;\n"; break;
              case 15: B += z ? " { float4 q=m0(s1,s2); v=(q.x+q.y+q.z+q.w).xxxx; }\n" : " v=dot(s1,s2).xxxx;\n"; break;
              case 16: B += z ? " { float4 q=m0(s1,s2); v=(q.x+q.y+q.z).xxxx; }\n" : " v=dot(s1.xyz,s2.xyz).xxxx;\n"; break;
              case 17: B += z ? " { float4 q=m0(s1,s2); v=(q.x+q.y+s3.x).xxxx; }\n" : " v=(dot(s1.xy,s2.xy)+s3.x).xxxx;\n"; break;
              case 18:
                B += " { float3 dd=float3(s1.z,s1.w,s1.x); float3 aa=abs(dd); float tc,sc,ma,fc;\n"
                     "   if(aa.z>=aa.x&&aa.z>=aa.y){tc=-dd.y;sc=dd.z<0?-dd.x:dd.x;ma=2*dd.z;fc=dd.z<0?5:4;}\n"
                     "   else if(aa.y>=aa.x){tc=dd.y<0?-dd.z:dd.z;sc=dd.x;ma=2*dd.y;fc=dd.y<0?3:2;}\n"
                     "   else{tc=-dd.y;sc=dd.x<0?dd.z:-dd.z;ma=2*dd.x;fc=dd.x<0?1:0;}\n"
                     "   v=float4(tc,sc,ma,fc); }\n";
                break;
              case 19: B += " v=max(max(s1.x,s1.y),max(s1.z,s1.w)).xxxx;\n"; break;
              case 20: case 21: case 22: case 23: {
                const char* o = vop == 20 ? "==" : vop == 21 ? "!=" : vop == 22 ? ">" : ">=";
                // src0 is the outer nesting counter (0 = active), src1 is the value tested
                B += S(" p=(s1.w==0.0)&&(s2.w%s0.0);\n v=((s1.x==0.0)&&(s2.x%s0.0))?0.0:(s1.x+1.0).xxxx;\n", o, o);
                break; }
              case 24: case 25: case 26: case 27: {
                const char* o = vop == 24 ? "==" : vop == 25 ? ">" : vop == 26 ? ">=" : "!=";
                if (!isVs) B += S(" if(any(s1%ss2)) discard;\n", o);
                B += S(" v=any(s1%ss2)?1.0:0.0;\n", o);
                break; }
              case 28: B += " v=float4(1.0,s1.y*s2.y,s1.z,s2.w);\n"; break;
              case 29: B += " a0=clamp((int)floor(s1.w+0.5),-256,255); v=max(s1,s2);\n"; break;
              default: logf_once(S("vop%u", vop).c_str(), "unknown vector opcode %u", vop); break;
            }
            if (vclamp) B += " v=saturate(v);\n";
        }
        bool sNeeded = sop != 50 || sRes;
        if (sNeeded) {
            uint32_t sw = bits(d1, 0, 8);
            bool neg = bits(d1, 24, 1);
            std::string ng = neg ? "-" : "";
            std::string base = src_base(d, 3);
            int ca = (((sw >> 6) & 3) + 3) & 3, cb = (sw & 3) & 3;
            if (sop >= 42 && sop <= 47) {
                // mulsc/addsc/subsc: the constant comes from src3, the temp index is packed in opcode bit 0, src3_sel and the swizzle
                uint32_t treg = (sop & 1) | (bits(d2, 29, 1) << 1) | (sw & 0x3C);
                uint32_t creg = bits(d2, 0, 8);
                bool c0 = bits(d1, 31, 1), c1 = bits(d1, 30, 1), arel = bits(d1, 29, 1);
                bool s1t = bits(d2, 31, 1), s2t = bits(d2, 30, 1);
                bool addressed = (s1t && s2t) ? c0 : c1;
                int idx = creg + (isVs ? 0 : 256);
                std::string ce = addressed ? S("c[clamp(%s+%d,0,511)]", arel ? "a0" : "aL", idx) : S("c[%d]", idx);
                if (bits(d0, 7, 1)) ce = "abs(" + ce + ")";
                std::string te = R(treg);
                if (bits(d0, 7, 1)) te = "abs(" + te + ")";
                B += " float a=" + ng + "(" + ce + S(".%c", comp(ca)) + "), b=" + ng + "(" + te + S(".%c", comp(cb)) + "), sr=0;\n";
            } else {
                B += " float a=" + ng + "(" + base + S(".%c", comp(ca)) + "), b=" + ng + "(" + base + S(".%c", comp(cb)) + "), sr=0;\n";
            }
            const char* FM = "3.4028235e38";
            switch (sop) {
              case 0: case 44: case 45: B += " sr=a+b;\n"; break;
              case 1: B += " sr=a+ps;\n"; break;
              case 2: case 42: case 43: B += getenv("ASURA_IEEEMUL") ? " sr=a*b;\n" : " sr=m0(a,b);\n"; break;
              case 3: B += getenv("ASURA_IEEEMUL") ? " sr=a*ps;\n" : " sr=m0(a,ps);\n"; break;
              case 4: B += S(" sr=(ps==-%s||isinf(ps)||isnan(ps)||isnan(b)||b<=0.0)?-%s:a*ps;\n", FM, FM); break;
              case 5: B += (exact && ca == cb) ? " sr=a;\n" : " sr=max(a,b);\n"; break;
              case 6: B += " sr=min(a,b);\n"; break;
              case 7: B += " sr=(a==0.0)?1.0:0.0;\n"; break;
              case 8: B += " sr=(a>0.0)?1.0:0.0;\n"; break;
              case 9: B += " sr=(a>=0.0)?1.0:0.0;\n"; break;
              case 10: B += " sr=(a!=0.0)?1.0:0.0;\n"; break;
              case 11: B += " sr=frac(a);\n"; break;
              case 12: B += " sr=trunc(a);\n"; break;
              case 13: B += " sr=floor(a);\n"; break;
              case 14: B += " sr=pow(2.0,a);\n"; break;
              case 15: B += S(" sr=log2(abs(a)); if(isinf(sr)||isnan(sr)) sr=-%s;\n", FM); break;
              case 16: B += " sr=log2(abs(a));\n"; break;
              case 17: B += S(" sr=clamp(1.0/a,-%s,%s);\n", FM, FM); break;
              case 18: B += " sr=1.0/a; if(isinf(sr)||isnan(sr)) sr=0.0;\n"; break;
              case 19: B += " sr=1.0/a;\n"; break;
              case 20: B += S(" sr=clamp(1.0/sqrt(abs(a)),-%s,%s);\n", FM, FM); break;
              case 21: B += " sr=1.0/sqrt(abs(a)); if(isinf(sr)||isnan(sr)) sr=0.0;\n"; break;
              case 22: B += " sr=1.0/sqrt(abs(a));\n"; break;
              case 23: B += " a0=clamp((int)floor(a+0.5),-256,255); sr=max(a,b);\n"; break;
              case 24: B += " a0=clamp((int)floor(a),-256,255); sr=max(a,b);\n"; break;
              case 25: case 46: case 47: B += " sr=a-b;\n"; break;
              case 26: B += " sr=a-ps;\n"; break;
              case 27: B += " p=(a==0.0); sr=p?0.0:1.0;\n"; break;
              case 28: B += " p=(a!=0.0); sr=p?0.0:1.0;\n"; break;
              case 29: B += " p=(a>0.0); sr=p?0.0:1.0;\n"; break;
              case 30: B += " p=(a>=0.0); sr=p?0.0:1.0;\n"; break;
              case 31: B += " if(a==0.0){p=false;sr=1.0;}else if(a==1.0){p=true;sr=0.0;}else{p=false;sr=a;}\n"; break;
              case 32: B += " sr=a-1.0; if(sr<=0.0){p=true;sr=0.0;}else{p=false;}\n"; break;
              case 33: B += S(" p=false; sr=%s;\n", FM); break;
              case 34: B += " p=(a==0.0); sr=a;\n"; break;
              case 35: case 36: case 37: case 38: case 39: {
                const char* cnd = sop == 35 ? "a==0.0" : sop == 36 ? "a>0.0" : sop == 37 ? "a>=0.0" : sop == 38 ? "a!=0.0" : "a==1.0";
                if (!isVs) B += S(" if(%s) discard;\n", cnd);
                B += S(" sr=(%s)?1.0:0.0;\n", cnd);
                break; }
              case 40: B += " sr=sqrt(a);\n"; break;
              case 48: B += " sr=sin(a);\n"; break;
              case 49: B += " sr=cos(a);\n"; break;
              case 50: B += " sr=ps;\n"; break;
              default: logf_once(S("sop%u", sop).c_str(), "unknown scalar opcode %u", sop); break;
            }
            if (sclamp) B += " sr=saturate(sr);\n";
        }
        auto maskstr = [](uint32_t m) { std::string s; for (int i = 0; i < 4; i++) if (m & (1u << i)) s += comp(i); return s; };
        if (exp) {
            std::string dst;
            if (isVs) {
                if (vdst == 62) dst = "oPos";
                else if (vdst == 63) dst = "oPts";
                else if (vdst < 16) dst = dynExp >= 0 ? S("oI[vsx[%d].%c]", dynExp >> 2, comp(dynExp & 3)) : S("oI[%u]", vdst);
                else if (vdst == 32) { dst = "eA"; memexport = true; }                   // memexport: address (see mx_flush)
                else if (vdst >= 33 && vdst <= 37) { dst = S("eM[%u]", vdst - 33); memexport = true; }   // memexport: data
                else { logf_once(S("vexp%u", vdst).c_str(), "ignored VS export register %u", vdst); }
            } else {
                if (vdst < 4) dst = S("oC[%u]", vdst);
                else if (vdst == 61) { dst = "oDepth"; writesDepth = true; }
                else { logf_once(S("pexp%u", vdst).c_str(), "ignored PS export register %u", vdst); }
            }
            if (!dst.empty()) {
                if (dst == "oDepth") {
                    if (vRes & 1) B += " oDepth=v.x;\n"; else if (sRes & 1) B += " oDepth=sr;\n";
                } else {
                    if (vRes) B += " " + dst + "." + maskstr(vRes) + "=v." + maskstr(vRes) + ";\n";
                    if (sRes) B += " " + dst + "." + maskstr(sRes) + "=sr;\n";
                    if (c0Mask) B += " " + dst + "." + maskstr(c0Mask) + "=0.0;\n";
                    if (c1Mask) B += " " + dst + "." + maskstr(c1Mask) + "=1.0;\n";
                    if (dst[0] == 'e' && dst[1] == 'M') B += S(" emw|=%uu;\n", 1u << (vdst - 33));
                }
            }
        } else {
            if (vRes) B += " " + Rrel(vdst, vrel) + "." + maskstr(vRes) + "=v." + maskstr(vRes) + ";\n";
            if (sRes) B += " " + Rrel(sdst, srel) + "." + maskstr(sRes) + "=sr;\n";
        }
        if (sNeeded) B += " ps=sr;\n";
        B += "}\n";
    }

    void emit_vfetch(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        bool mini = bits(d1, 30, 1);
        if (!mini) memcpy(prevFull, d, 12);
        const uint32_t* f = mini ? prevFull : d;
        uint32_t srcReg = bits(f[0], 5, 6), srcSw = bits(f[0], 30, 2), slot = bits(f[0], 20, 5) * 3 + bits(f[0], 25, 2);
        uint32_t stride = bits(f[2], 0, 8);
        bool rounded = bits(f[1], 15, 1);
        uint32_t dst = bits(d0, 12, 6), dsw = bits(d1, 0, 12), fmt = bits(d1, 16, 6);
        bool isSigned = bits(d1, 12, 1), isInt = bits(d1, 13, 1), rfNoZero = bits(d1, 14, 1);
        int exp = sext(bits(d1, 24, 6), 6);
        int offset = sext(bits(d2, 8, 23), 23);
        bool pred = bits(d1, 31, 1), predc = bits(d2, 31, 1);
        if (dynVs) {
            const uint32_t f = (uint32_t)dynFetchPos.size();
            dynFetchPos.push_back((uint32_t)(d - ucode));
            std::string& B = body;
            if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
            const std::string ix = R(srcReg) + S(".%c", comp(srcSw));
            B += S("{ uint4 P=vsf[%u]; float ix=%s; if(P.x&0x8000000u) ix=round(ix);\n"
                   " float4 fv=vfd(vfo[(P.x&127u)>>2][P.x&3u]+(uint)((int)ix)*((P.x>>8)&255u)*4u+(uint)((int)P.y*4), (P.x>>16)&63u, P.x>>24);\n"
                   " int ex=(int)(P.z<<14)>>26; if(ex!=0) fv*=exp2((float)ex);\n"
                   " uint dr=(P.z>>18)&63u; float4 t=@VFR@;\n"
                   " [unroll] for(uint c=0;c<4;c++){ uint sw=(P.z>>(3*c))&7u; if(sw!=7u) t[c]=sw<4u?fv[sw]:(sw==5u?1.0:0.0); }\n"
                   " @VFW@ }\n", f, ix.c_str());
            // The game moves fetches between the vertex input registers per mesh (r2 -> r4 ...), so the register is chosen at draw time, but only among
            // the registers this shader's own fetches write (the markers become selects over them in translate_shader). Writing r[dr] instead made the
            // whole register file an indexed array: about 3x the compile time and twice the code.
            R(dst); dynDstMask |= 1ull << (dst & 63);
            return;
        }
        auto it = vfs.find(slot);
        if (it == vfs.end() || it->second < stride) vfs[slot] = stride;
        std::string& B = body;
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        std::string idxExpr = R(srcReg) + S(".%c", comp(srcSw));
        B += S(" uint vi=(uint)((int)%s(%s)); uint va=vfo[%u].%c+vi*%uu+%du; uint w0=VB.Load(va), w1=0, w2=0, w3=0; float4 fv=float4(0,0,0,1);\n",
               rounded ? "round" : "", idxExpr.c_str(), slot >> 2, comp(slot & 3), stride * 4, offset * 4);
        // integer component extraction: x = (w >> off) & mask, sign/normalise as requested
        auto intcomp = [&](const char* dw, int off, int bitsw) {
            std::string v;
            if (isSigned) v = S("float((int)(%s<<%d)>>%d)", dw, 32 - off - bitsw, 32 - bitsw);
            else v = S("float((%s>>%d)&%uu)", dw, off, bitsw == 32 ? 0xFFFFFFFFu : ((1u << bitsw) - 1));
            if (!isInt) {
                float umax = (float)(bitsw == 32 ? 4294967295.0 : (double)((1u << bitsw) - 1));
                float smax = (float)(bitsw == 32 ? 2147483647.0 : (double)((1u << (bitsw - 1)) - 1));
                if (isSigned) v = rfNoZero ? S("((2.0*%s+1.0)/%.1f)", v.c_str(), umax) : S("max(%s/%.1f,-1.0)", v.c_str(), smax);
                else v = S("(%s/%.1f)", v.c_str(), umax);
            }
            return v;
        };
        auto setc = [&](char c, const std::string& e) { B += S(" fv.%c=", c) + e + ";\n"; };
        auto f16 = [](const char* dw, int off) { return S("f16tof32((%s>>%d)&0xFFFFu)", dw, off); };
        switch (fmt) {
          case 6: setc('x', intcomp("w0", 0, 8)); setc('y', intcomp("w0", 8, 8)); setc('z', intcomp("w0", 16, 8)); setc('w', intcomp("w0", 24, 8)); break;
          case 7: setc('x', intcomp("w0", 0, 10)); setc('y', intcomp("w0", 10, 10)); setc('z', intcomp("w0", 20, 10)); setc('w', intcomp("w0", 30, 2)); break;
          case 16: setc('x', intcomp("w0", 0, 11)); setc('y', intcomp("w0", 11, 11)); setc('z', intcomp("w0", 22, 10)); break;
          case 17: setc('x', intcomp("w0", 0, 10)); setc('y', intcomp("w0", 10, 11)); setc('z', intcomp("w0", 21, 11)); break;
          case 25: setc('x', intcomp("w0", 0, 16)); setc('y', intcomp("w0", 16, 16)); break;
          case 26: B += " w1=VB.Load(va+4);\n"; setc('x', intcomp("w0", 0, 16)); setc('y', intcomp("w0", 16, 16)); setc('z', intcomp("w1", 0, 16)); setc('w', intcomp("w1", 16, 16)); break;
          case 31: setc('x', f16("w0", 0)); setc('y', f16("w0", 16)); break;
          case 32: B += " w1=VB.Load(va+4);\n"; setc('x', f16("w0", 0)); setc('y', f16("w0", 16)); setc('z', f16("w1", 0)); setc('w', f16("w1", 16)); break;
          case 33: setc('x', intcomp("w0", 0, 32)); break;
          case 34: B += " w1=VB.Load(va+4);\n"; setc('x', intcomp("w0", 0, 32)); setc('y', intcomp("w1", 0, 32)); break;
          case 35: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); w3=VB.Load(va+12);\n"; setc('x', intcomp("w0", 0, 32)); setc('y', intcomp("w1", 0, 32)); setc('z', intcomp("w2", 0, 32)); setc('w', intcomp("w3", 0, 32)); break;
          case 36: B += " fv.x=asfloat(w0);\n"; break;
          case 37: B += " w1=VB.Load(va+4); fv.xy=float2(asfloat(w0),asfloat(w1));\n"; break;
          case 57: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); fv.xyz=float3(asfloat(w0),asfloat(w1),asfloat(w2));\n"; break;
          case 38: B += " w1=VB.Load(va+4); w2=VB.Load(va+8); w3=VB.Load(va+12); fv=float4(asfloat(w0),asfloat(w1),asfloat(w2),asfloat(w3));\n"; break;
          default: logf_once(S("vfmt%u", fmt).c_str(), "unknown vertex format %u", fmt); break;
        }
        if (exp) B += S(" fv.xyzw*=%.9g;\n", std::ldexp(1.0, exp));
        for (int i = 0; i < 4; i++) {
            uint32_t s = (dsw >> (3 * i)) & 7;
            if (s == 7) continue;
            std::string val = s < 4 ? S("fv.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
            B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
        }
        B += "}\n";
    }

    void emit_tfetch(const uint32_t* d) {
        uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
        uint32_t op = bits(d0, 0, 5), src = bits(d0, 5, 6), dst = bits(d0, 12, 6), slot = bits(d0, 20, 5), srcSw = bits(d0, 26, 6);
        uint32_t dsw = bits(d1, 0, 12), dim = bits(d2, 14, 2);
        bool pred = bits(d1, 31, 1), predc = bits(d2, 31, 1);
        std::string& B = body;
        if (pred) B += S("if (p == %s) ", predc ? "true" : "false");
        B += "{\n";
        std::string sreg = R(src);
        if (op == 24) { B += " lodReg=" + sreg + S(".%c;\n", comp(srcSw & 3)) + "}\n"; return; }
        if (op == 25 || op == 26) { B += "}\n"; return; }                // setGradientH/V: unused for now
        if (op == 18) {                                                     // getGradients
            std::string g = isVs ? "float4(0,0,0,0)" : "float4(ddx(" + sreg + ".x),ddx(" + sreg + ".y),ddy(" + sreg + ".x),ddy(" + sreg + ".y))";
            B += " float4 t=" + g + ";\n";
            for (int i = 0; i < 4; i++) {
                uint32_t s = (dsw >> (3 * i)) & 7;
                if (s == 7) continue;
                std::string val = s < 4 ? S("t.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
                B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
            }
            B += "}\n"; return;
        }
        if (op == 19 && slot < 16) {                                        // getWeights: the bilinear weights a fetch here would use
            // The fractional texel position, frac(coord * size - 0.5), per axis, on the fetch's own grid (tfi = 1 / guest size; the texture is
            // bound so tfi is set). UE3's shadow filtering blends its point-sampled taps with these; without them the weights were whatever the
            // registers held, and shadows on lit surfaces came out as per-pixel speckle (the bronze figure and ship in episode 1).
            TexUse u{ slot, dim };
            if (texs.find(slot) == texs.end()) texs[slot] = u;
            const bool denorm = bits(d0, 25, 1);
            const float ox = sext(bits(d2, 16, 5), 5) * 0.5f, oy = sext(bits(d2, 21, 5), 5) * 0.5f, oz = sext(bits(d2, 26, 5), 5) * 0.5f;
            const std::string uvw = sreg + S(".%c%c%c", comp(srcSw & 3), comp((srcSw >> 2) & 3), comp((srcSw >> 4) & 3));
            if (dim == 3) B += " float4 t=0;\n";                                // cube: not seen yet
            else {
                B += " float3 uw=" + uvw + ";\n";
                B += denorm ? " float2 tc=uw.xy;\n" : S(" float2 tc=uw.xy/tfi[%u].xy;\n", slot);
                B += S(" tc+=float2(%g,%g);\n", ox, oy);
                B += S(" float4 t=float4(frac(tc-0.5),%s,0.0);\n", dim == 2 ? S("frac(uw.z*max(tfp[%u].w,1.0)-0.5+%g)", slot, oz).c_str() : "0.0");
                if (dim == 0) B += " t.y=0.0;\n";
            }
            for (int i = 0; i < 4; i++) {
                uint32_t s = (dsw >> (3 * i)) & 7;
                if (s == 7) continue;
                std::string val = s < 4 ? S("t.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
                B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
            }
            B += "}\n"; return;
        }
        if (op != 1 && op != 17) {
            logf_once(S("fop%u", op).c_str(), "unhandled fetch opcode %u", op);
            B += "}\n"; return;
        }
        if (slot >= 16) { logf_once("tf16", "texture fetch slot %u beyond sampler range", slot); B += "}\n"; return; }
        TexUse u{ slot, dim };
        uint32_t magf = bits(d1, 12, 2), minf = bits(d1, 14, 2), mipf = bits(d1, 16, 2);
        if (magf != 3) u.magf = magf;
        if (minf != 3) u.minf = minf;
        if (mipf != 3) u.mipf = mipf;
        auto it = texs.find(slot);
        if (it == texs.end()) texs[slot] = u;
        bool denorm = bits(d0, 25, 1), useRegLod = bits(d1, 29, 1), useCompLod = bits(d1, 28, 1);
        int bias = sext(bits(d2, 2, 7), 7);
        float ox = sext(bits(d2, 16, 5), 5) * 0.5f, oy = sext(bits(d2, 21, 5), 5) * 0.5f;
        std::string uvw = sreg + S(".%c%c%c", comp(srcSw & 3), comp((srcSw >> 2) & 3), comp((srcSw >> 4) & 3));
        std::string lod = useRegLod ? "lodReg" : "0.0";
        std::string smp;
        // Pixel shaders with loops used to sample level 0 everywhere (HLSL refuses gradients in some loops): fine detail on distant metal and clouds
        // then sparkled. Now they sample normally, and build_shader keeps a level-0 copy (to_level0) in case the compiler refuses.
        static const bool kLoopLod0 = getenv("ASURA_LOOPLOD0") != nullptr;
        bool explicitLod = isVs || (hasLoop && kLoopLod0) || useRegLod;
        if (dim == 3) {
            B += " float3 uv=cubedir(" + uvw + ");\n";
            smp = explicitLod ? S("TC[%u].SampleLevel(SS[%u],uv,%s)", slot, slot, lod.c_str()) : S("TC[%u].Sample(SS[%u],uv)", slot, slot);
        } else {
            B += " float3 uw=" + uvw + ";\n";
            B += " float2 uv=uw.xy;\n";
            if (dim == 0) B += " uv.y=0.5;\n";
            if (denorm) B += S(" uv*=tfi[%u].xy;\n", slot);
            else B += S(" uv*=tfscale(%u);\n", slot);
            if (ox != 0 || oy != 0) B += S(" uv+=float2(%g,%g)*tfi[%u].xy;\n", ox, oy, slot);
            auto one = [&](const std::string& c) {
                if (explicitLod) return S("T2[%u].SampleLevel(SS[%u],%s,%s)", slot, slot, c.c_str(), lod.c_str());
                if (bias) return S("T2[%u].SampleBias(SS[%u],%s,%g)", slot, slot, c.c_str(), bias / 16.0f);
                return S("T2[%u].Sample(SS[%u],%s)", slot, slot, c.c_str());
            };
            if (dim == 2) {
                // 3D or stacked fetch. Supported for resolved surfaces, whose slices sit one under another (tfp.w = depth,
                // tfi.z = slice stride in surface v): the two nearest slices, blended, with v kept inside the slice.
                B += S(" float ld=max(tfp[%u].w,1.0), lz=clamp(uw.z*ld-0.5,0.0,ld-1.0), l0=floor(lz), lf=lz-l0, l1=min(l0+1.0,ld-1.0);\n", slot);
                B += S(" uv.y=clamp(uv.y,0.5*tfscale(%u).y*tfi[%u].y,tfscale(%u).y*(1.0-0.5*tfi[%u].y));\n", slot, slot, slot, slot);
                // Bilinear on the game's own grid (gridlerp), as the console filters: the surface is stored at render scale, so each grid cell is a
                // scale x scale block of equal texels and hardware filtering only blended across the one-texel seam between blocks. The colour
                // table then graded in steps, more so the higher the scale. (Tried once on 2026-11-10 at 2x and judged worse, while the first row
                // of every slice still read the padding.) ASURA_LUTHW=1: the hardware filter, as before.
                static const bool kLutHw = getenv("ASURA_LUTHW") != nullptr;
                if (kLutHw) smp = "lerp(" + one(S("uv+float2(0,l0*tfi[%u].z)", slot)) + "," + one(S("uv+float2(0,l1*tfi[%u].z)", slot)) + ",lf)";
                else smp = S("lerp(gridlerp(T2[%u],SS[%u],uv,tfscale(%u),tfi[%u].xy,l0*tfi[%u].z),gridlerp(T2[%u],SS[%u],uv,tfscale(%u),tfi[%u].xy,l1*tfi[%u].z),lf)",
                             slot, slot, slot, slot, slot, slot, slot, slot, slot, slot);
            } else smp = one("uv");
        }
        if (op == 17) {                                                     // getCompTexLOD
            if (dim == 3 || explicitLod) B += " float4 t=0;\n";
            else B += S(" float4 t=T2[%u].CalculateLevelOfDetail(SS[%u],uv).xxxx;\n", slot, slot);
            if (useCompLod) {}
        } else {
            B += " float4 t=texfix(" + smp + S(",%u);\n", slot);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t s = (dsw >> (3 * i)) & 7;
            if (s == 7) continue;
            std::string val = s < 4 ? S("t.%c", comp(s)) : (s == 5 ? "1.0" : "0.0");
            B += " " + R(dst) + S(".%c=", comp(i)) + val + ";\n";
        }
        B += "}\n";
    }

    void emit_exec(uint32_t addr, uint32_t count, uint32_t seq) {
        for (uint32_t k = 0; k < count; k++, seq >>= 2) {
            uint32_t off = (addr + k) * 3;
            if (off + 3 > words) { logf_once("execoob", "exec beyond shader end"); return; }
            const uint32_t* d = ucode + off;
            if (seq & 1) {
                uint32_t op = d[0] & 0x1F;
                if (op == 0) emit_vfetch(d); else emit_tfetch(d);
            } else emit_alu(d);
        }
    }

    struct Cf { uint32_t w0, w1, op; };
    std::vector<Cf> cfs;

    void decode_cf() {
        uint32_t bound = words / 3;
        for (uint32_t i = 0; i < bound && (i + 1) * 3 <= words; i++) {
            const uint32_t* t = ucode + i * 3;
            Cf a{ t[0], t[1] & 0xFFFF, 0 }, b{ (t[1] >> 16) | (t[2] << 16), t[2] >> 16, 0 };
            a.op = a.w1 >> 12; b.op = b.w1 >> 12;
            for (Cf* c : { &a, &b }) {
                cfs.push_back(*c);
                if ((c->op >= 1 && c->op <= 6) || c->op == 13 || c->op == 14) bound = std::min(bound, c->w0 & 0xFFF);
            }
        }
    }

    void walk() {
        // Is this straight-line code? (no jumps, calls, or loops)
        bool jumps = false;
        for (auto& c : cfs) if (c.op == 9 || c.op == 11 || c.op == 7) jumps = true;
        for (auto& c : cfs) if (c.op == 7) hasLoop = true;
        body.clear(); dynFetchPos.clear(); dynExpPos.clear(); dynDstMask = 0;
        std::string& B = body;
        std::vector<std::pair<uint32_t, std::string>> loopEnds;
        for (uint32_t i = 0; i < cfs.size(); i++) {
            const Cf& c = cfs[i];
            uint32_t w0 = c.w0, w1 = c.w1, op = c.op;
            std::string guard = jumps ? S("if (pc <= %u) ", i) : "";
            auto boolCond = [&](uint32_t b, bool cond) {
                return S("(((bools[%u].%c>>%u)&1)==%uu)", b >> 7, comp((b >> 5) & 3), b & 31, cond ? 1 : 0);
            };
            switch (op) {
              case 12: if (isVs) B += "/*mx*/" + guard + "MX_FLUSH();\n"; break;   // alloc: ends the previous memexport block; the line is removed from shaders without memexport
              case 0: case 15: break;                                       // nop, mark fetch done
              case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
                uint32_t addr = w0 & 0xFFF, cnt = (w0 >> 12) & 7, seq = (w0 >> 16) & 0xFFF;
                bool isEnd = op == 2 || op == 4 || op == 6 || op == 14;
                std::string cond;
                if (op == 3 || op == 4 || op == 13 || op == 14) cond = boolCond((w1 >> 2) & 0xFF, (w1 >> 10) & 1);
                else if (op == 5 || op == 6) cond = S("(p==%s)", ((w1 >> 10) & 1) ? "true" : "false");
                B += guard + "{\n";
                if (!cond.empty()) B += "if " + cond + " {\n";
                emit_exec(addr, cnt, seq);
                if (!cond.empty()) B += "}\n";
                if (isEnd && jumps) B += "pc=0x7FFFFFFF;\n";
                B += "}\n";
                if (isEnd && !jumps) goto done;
                break; }
              case 7: {                                                     // loop start
                uint32_t id = (w0 >> 16) & 31;
                int u = uniq++;
                B += guard + S("{}\n{ uint lc=loops[%u].%c; int lcnt=lc&255; aL=(int)(lc<<16)>>24; int lstep=(int)(lc<<8)>>24; int li%d=0;\n"
                               "[loop] while(li%d<lcnt) {\n", id >> 2, comp(id & 3), u, u);
                loopEnds.push_back({ i, S("li%d++; aL+=lstep; if(li%d>=lcnt) break;\n}\n}\n", u, u) });
                break; }
              case 8: {                                                     // loop end
                bool brk = (w0 >> 21) & 1, cnd = (w1 >> 10) & 1;
                if (brk) B += S("if (p==%s) break;\n", cnd ? "true" : "false");
                if (!loopEnds.empty()) { B += loopEnds.back().second; loopEnds.pop_back(); }
                break; }
              case 9: case 10:
                logf_once("cfcall", "shader uses call/return: not implemented");
                break;
              case 11: {                                                    // cond jump
                uint32_t target = w0 & 0x1FFF;
                bool uncond = (w0 >> 13) & 1, predd = (w0 >> 14) & 1;
                std::string cond;
                if (uncond) cond = "(true)";
                else if (predd) cond = S("(p==%s)", ((w1 >> 10) & 1) ? "true" : "false");
                else cond = boolCond((w1 >> 2) & 0xFF, (w1 >> 10) & 1);
                if (target <= i) logf_once("backjmp", "backward jump in shader: not implemented");
                B += guard + "if " + cond + S(" pc=%u;\n", target);
                break; }
              default: break;
            }
        }
    done:
        while (!loopEnds.empty()) { B += loopEnds.back().second; loopEnds.pop_back(); }
    }

    std::string decls() {
        std::string d;
        if (useArray) d += (isVs && !getenv("ASURA_VSIMPRECISE")) ? " precise float4 r[64]; for(int ri=0;ri<64;ri++) r[ri]=0;\n" : " float4 r[64]; for(int ri=0;ri<64;ri++) r[ri]=0;\n";
        else for (int n : usedRegs) d += S(isVs && !getenv("ASURA_VSIMPRECISE") ? " precise float4 r%d=0;\n" : " float4 r%d=0;\n", n);
        d += " bool p=false; int a0=0; int aL=0; float ps=0.0; float lodReg=0.0; int pc=0;\n";
        if (isVs && memexport) d += " float4 eA=0; float4 eM[5]; for(int ei=0;ei<5;ei++) eM[ei]=0; uint emw=0;\n";
        return d;
    }
};

static std::string uv_helper() {
    return "#define tfscale(n) tfp[n].xy\n";     // normalised lookups multiply by the uv scale; set per bind
}

static void shader_record(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen);
// ASURA_SHADERFIND=<n>: for new shaders after the first n, find every copy of their microcode in guest memory and log the
// 128 bytes before each, to see the form the game unpacked them in (for the shader index). Six shaders, then it stops.
static void shader_find(const std::vector<uint32_t>& code, bool isVs) {
    static const char* env = getenv("ASURA_SHADERFIND");
    static int seen = 0, done = 0;
    if (!env || code.size() < 16 || done >= 6 || seen++ < atoi(env)) return;
    done++;
    uint32_t pat[8]; for (int k = 0; k < 8; k++) pat[k] = _byteswap_ulong(code[k]);   // as stored in guest memory
    int hits = 0;
    fprintf(stderr, "[shaderfind] %s of %zu words, first words %08X %08X %08X %08X\n", isVs ? "vs" : "ps", code.size(), code[0], code[1], code[2], code[3]);
    for (uint64_t off = 0; off < 0x100000000ull; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + off, &mi, sizeof mi)) break;
        const uint64_t size = mi.RegionSize - (uint64_t)((g_base + off) - (uint8_t*)mi.BaseAddress);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && size >= 32) {
            const uint32_t* w = (const uint32_t*)(g_base + off);
            for (uint64_t i = 0; i + 8 <= size / 4; i++) {
                if (w[i] != pat[0] || memcmp(&w[i], pat, 32)) continue;
                const uint32_t ga = (uint32_t)(off + i * 4);
                std::string s = S("[shaderfind]   at %08X, before:", ga);
                for (int k = ga < 0x80000000u ? -256 : -8; k < 0; k++) if ((int64_t)i + k >= 0) s += S(" %08X", _byteswap_ulong(w[i + k]));
                fprintf(stderr, "%s\n", s.c_str());
                if (++hits >= 8) break;
            }
        }
        if (hits >= 8) break;
        off += size;
    }
    fprintf(stderr, "[shaderfind]   %d copies\n", hits);
}
// Microcode to HLSL (src; fallback: the level-0 sampling copy for loop shaders, empty if not needed), filling in sh's texture and vertex-fetch
// use. h is the shader's key (debug switches name shaders by it). Thread-safe: also used by the background pixel-shader warm-up.
static void translate_shader(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen, uint64_t h, XShader* sh, std::string& src, std::string& fallback, bool dyn = false) {
    Translator T; T.ucode = code.data(); T.words = (uint32_t)code.size(); T.isVs = isVs; T.out = sh;
    if (dyn) T.dynVs = true;                                                // fetches write their own register, so no register array is needed
    T.decode_cf();
    T.walk();
    if (T.needRel) { T.useArray = true; T.usedRegs.clear(); T.needRel = false; memset(T.prevFull, 0, sizeof(T.prevFull)); T.walk(); }
    if (T.memexport) { T.exact = true; T.usedRegs.clear(); T.needRel = false; memset(T.prevFull, 0, sizeof(T.prevFull)); T.walk(); }
    sh->memexport = T.memexport;
    if (T.dynVs) {   // generic VS fetch writes: read and write the register the draw names, among the fetches' own registers
        std::string rd, wr;
        for (int r = 63; r >= 0; r--) if (T.dynDstMask >> r & 1) {
            const std::string reg = T.R(r);
            rd = rd.empty() ? reg : S("(dr==%du?%s:%s)", r, reg.c_str(), rd.c_str());
            wr += S("if(dr==%du) %s=t; ", r, reg.c_str());
        }
        auto subst = [&](const std::string& m, const std::string& by) { for (size_t at; (at = T.body.find(m)) != std::string::npos;) T.body.replace(at, m.size(), by); };
        subst("@VFR@", rd.empty() ? "float4(0,0,0,0)" : rd);
        subst("@VFW@", wr);
    }
    if (T.memexport) fprintf(stderr, "[mx] memexport shader %016llX (%u dwords)\n", (unsigned long long)h, T.words);
    {   // alloc markers: the memexport flush in memexport shaders, nothing (no line at all, so the source and its cached compile stay the same) elsewhere
        std::string b; b.reserve(T.body.size());
        size_t pos = 0;
        while (pos < T.body.size()) {
            size_t e = T.body.find('\n', pos); if (e == std::string::npos) e = T.body.size() - 1;
            if (T.body.compare(pos, 6, "/*mx*/") == 0) {
                if (T.memexport) { std::string l = T.body.substr(pos + 6, e + 1 - pos - 6); size_t k = l.find("MX_FLUSH();"); if (k != std::string::npos) l.replace(k, 11, kMxFlush); b += l; }
            }
            else b.append(T.body, pos, e + 1 - pos);
            pos = e + 1;
        }
        T.body.swap(b);
    }
    src = std::string(kCommon) + uv_helper() + kVOut;
    if (dyn) src += kVfd;
    if (T.memexport) src += kMemExport;
    if (isVs && !getenv("ASURA_VSIMPRECISE")) {
        // Every vertex shader temporary is precise: a depth pre-pass and the colour pass after it run the same microcode in different shaders, and
        // compiler or driver freedom (fusing a multiply-add in one, not the other) moved depth by a bit, so a GEQUAL/EQUAL test failed on scattered
        // pixels and the background showed through (speckled bronze armour in episode 1).
        auto prec = [](std::string& s) {
            for (const char* t : { " float4 ", " float " }) {
                const std::string from = t, to = std::string(" precise") + t;
                for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
            }
        };
        prec(T.body);
    }
    if (isVs) {
        // VTE: optionally divide-undo, then map guest screen space onto the full host render target
        src += "VOut main(uint vid : SV_VertexID) {\n float4 oI[16]; for(int i=0;i<16;i++) oI[i]=0; float4 oPos=float4(0,0,0,1), oPts=0;\n";
        src += T.decls();
        if (T.useArray) src += " r[0].x=(float)((int)vid+(int)vpW.y);\n";
        else if (T.usedRegs.count(0)) src += " r0.x=(float)((int)vid+(int)vpW.y);\n";
        src += T.body;
        if (T.memexport) src += std::string(" ") + kMxFlush + "\n";
        src += " VOut o; for(int i=0;i<16;i++) o.o[i]=oI[i];\n"
               " float w=oPos.w; if(vpW.x==0.0) w=1.0/w;\n"
               " float2 xy=oPos.xy; if(vpZ.z!=0.0) xy*=w;\n"
               " float z=oPos.z; if(vpZ.w!=0.0) z*=w;\n"
               // precise: separate shaders drawing the same mesh must produce identical z for depth EQUAL passes
               " precise float4 finalPos=float4(xy*vpNdc.xy+w*vpNdc.zw, z*vpZ.x+w*vpZ.y, w);\n"
               // 2D art is authored half a pixel off (-0.5 to 719.5). Above render scale 1 the half host pixel falls short of the edge and loses the last row and column.
               " if(vpSnap.x>0.0 && w==1.0){ float2 g=(finalPos.xy*float2(0.5,-0.5)+0.5)*vpSnap.xy-vpSnap.w; float2 f=abs(frac(g)-0.5);\n"
               "  finalPos.xy+=float2(f.x<0.016?vpSnap.z*2.0/vpSnap.x:0.0, f.y<0.016?-vpSnap.z*2.0/vpSnap.y:0.0); }\n"
               " o.pos=finalPos;\n"
               " return o; }\n";
    } else {
        src += "struct POut { float4 c0:SV_Target0; float4 c1:SV_Target1; float4 c2:SV_Target2; float4 c3:SV_Target3;";
        if (T.writesDepth) src += " float d:SV_Depth;";
        src += " };\nPOut main(VOut In) {\n float4 oC[4]; for(int i=0;i<4;i++) oC[i]=float4(0,0,0,0); float oDepth=In.pos.z;\n";
        // Interpolators arrive in r0..r15. Registers used only as temps get harmless initial values.
        std::string d = T.decls();
        src += d;
        // vpW.z set (small targets above render scale 1): interpolators are moved from the host pixel's centre to the guest pixel's,
        // along their screen derivatives, so lookup-table passes compute the values the game laid out per guest pixel.
        // A hair past the centre (+0.02 guest pixel): the colour table's shader picks slice and row from these values, and at the exact centre the
        // first row of each slice sat on the boundary with the slice before. Which side won depended on the render scale (fine at 2, green at 3).
        src += S(" float2 gd=(floor(In.pos.xy/%d.0)+0.52)*%d.0-In.pos.xy; if(vpW.z==0.0) gd=0;\n", g_resScale, g_resScale);
        for (int i = 0; i < 16; i++) {
            if (T.useArray) src += S(" r[%d]=In.o[%d]+ddx_fine(In.o[%d])*gd.x+ddy_fine(In.o[%d])*gd.y;\n", i, i, i, i);
            else if (T.usedRegs.count(i)) src += S(" r%d=In.o[%d]+ddx_fine(In.o[%d])*gd.x+ddy_fine(In.o[%d])*gd.y;\n", i, i, i, i);
        }
        if (paramGen < 16) {
            // The game expects guest pixels. vpW.z set: the guest pixel's centre, for small targets (lookup tables) whose shaders
            // turn the position into integer rows and slices; otherwise the host pixel's own position, for finer upscaled passes.
            std::string pos = S("(vpW.z!=0.0?floor(In.pos.xy/%d.0)+0.5:In.pos.xy/%d.0)", g_resScale, g_resScale);
            if (T.useArray) src += S(" r[%u]=float4(%s,0,0);\n", paramGen, pos.c_str());
            else if (T.usedRegs.count(paramGen)) src += S(" r%u=float4(%s,0,0);\n", paramGen, pos.c_str());
        }
        // ASURA_DBGPSAT="<ps hash>@<marker>#<k>:<hlsl float4 expr>": stop that pixel shader right after the k-th line of its body containing
        // <marker> and output <expr>, to look at a value the shader overwrites before its end (DBGPS only sees the end).
        if (const char* at = getenv("ASURA_DBGPSAT")) {
            const std::string spec = at, hs = S("%016llX@", (unsigned long long)h);
            if (spec.rfind(hs, 0) == 0) {
                const size_t hashEnd = hs.size(), colon = spec.find(':', hashEnd), sharp = spec.rfind('#', colon);
                if (colon != std::string::npos && sharp != std::string::npos && sharp > hashEnd) {
                    const std::string marker = spec.substr(hashEnd, sharp - hashEnd), expr = spec.substr(colon + 1);
                    const int k = atoi(spec.c_str() + sharp + 1);
                    int seen = 0; size_t pos = 0;
                    while ((pos = T.body.find(marker, pos)) != std::string::npos) {
                        const size_t eol = T.body.find('\n', pos);
                        if (++seen == k && eol != std::string::npos) {
                            T.body.insert(eol + 1, " { POut dbg; dbg.c0=" + expr + "; dbg.c1=0; dbg.c2=0; dbg.c3=0; return dbg; }\n");
                            fprintf(stderr, "[gfx] debug probe on ps %s after '%s' #%d\n", hs.c_str(), marker.c_str(), k);
                            break;
                        }
                        pos = eol == std::string::npos ? T.body.size() : eol;
                    }
                }
            }
        }
        src += T.body;
        src += " uint af=(uint)alpha.x; if(af!=7){ float aa=oC[0].a, rf=alpha.y; bool atok=true;\n"
               "  if(af==0) atok=false; else if(af==1) atok=aa<rf; else if(af==2) atok=aa==rf; else if(af==3) atok=aa<=rf;\n"
               "  else if(af==4) atok=aa>rf; else if(af==5) atok=aa!=rf; else if(af==6) atok=aa>=rf; if(!atok) discard; }\n";
        // 8_8_8_8_GAMMA targets store piecewise-linear gamma (colour only). Inverse of the ramp in texfix.
        src += " if(alpha.z!=0.0){ float3 lc=saturate(oC[0].rgb);\n"
               "  oC[0].rgb=(lc<0.0625)?lc*4.0:((lc<0.125)?lc*2.0+0.125:((lc<0.5)?lc+0.25:lc*0.5+0.5)); }\n";
        // Colour exponent bias of the target (RB_COLOR_INFO 20-25): output times 2^bias. Negative alpha.w also clamps to -32..32,
        // the range of the 16_16 / 16_16_16_16 targets (kept as half floats).
        src += " if(alpha.w!=0.0) { oC[0]*=abs(alpha.w); if(alpha.w<0.0) oC[0]=clamp(oC[0],-32.0,32.0); }\n";
        // vpW.w: the target is the console's 2_10_10_10_FLOAT (7e3), held here as 16-bit float. The console's format can't hold more than
        // 31.875, nor NaN or infinity (NaN becomes 0); without this, a lighting shader's occasional huge or NaN value survived as a white
        // speck after bloom and tone mapping (the planet at the start of episode 1, Asura's armour).
        src += " if(vpW.w>0.5) { oC[0].rgb=min(max(oC[0].rgb,0.0),31.875); oC[0].a=saturate(oC[0].a); }\n";
        // BCR2_DBGPS="<ps hash>:<hlsl float4 expr>" replaces that pixel shader's first output, to view one input at a time
        if (const char* dbg = getenv("ASURA_DBGPS")) {
            std::string ds = dbg, hs = S("%016llX:", (unsigned long long)h);
            if (ds.rfind(hs, 0) == 0) { src += " oC[0]=" + ds.substr(hs.size()) + ";\n"; fprintf(stderr, "[gfx] debug override on ps %s\n", hs.c_str()); }
        }
        if (const char* tint = getenv("ASURA_TINTPS")) {                     // "hash,hash,...": paint each listed pixel shader a flat colour (red, green, blue, yellow, magenta, cyan, white, orange)
            static const char* kTint[8] = { "4,0,0", "0,4,0", "0,0,4", "4,4,0", "4,0,4", "0,4,4", "4,4,4", "4,1.5,0" };
            const std::string hs = S("%016llX", (unsigned long long)h);
            int idx = 0;
            for (const char* p = tint; *p; idx++) {
                const char* e = strchr(p, ','); const size_t n = e ? (size_t)(e - p) : strlen(p);
                if (n == hs.size() && _strnicmp(p, hs.c_str(), n) == 0) { src += S(" oC[0]=float4(%s,1);\n", kTint[idx & 7]); fprintf(stderr, "[gfx] tint %d on ps %s\n", idx, hs.c_str()); break; }
                if (!e) break;
                p = e + 1;
            }
        }
        src += " POut o; o.c0=oC[0]; o.c1=oC[1]; o.c2=oC[2]; o.c3=oC[3];";
        if (T.writesDepth) src += " o.d=oDepth;";
        src += " return o; }\n";
    }
    if (const char* dd = getenv("ASURA_SHADERDUMP")) {
        std::string p = std::string(dd) + S("\\%s_%016llX.hlsl", isVs ? "vs" : "ps", (unsigned long long)h);
        if (FILE* f = fopen(p.c_str(), "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        // the microcode too, as text (one dword per line), to check a translation against the original
        std::string m = std::string(dd) + S("\\%s_%016llX.ucode.txt", isVs ? "vs" : "ps", (unsigned long long)h);
        if (FILE* f = fopen(m.c_str(), "wb")) { for (uint32_t w : code) fprintf(f, "%08X\n", w); fclose(f); }
    }
    for (auto& kv : T.texs) sh->tex.push_back(kv.second);
    for (auto& kv : T.vfs) sh->vf.push_back({ kv.first, kv.second });
    if (dyn) { sh->fetchPos = T.dynFetchPos; sh->expPos = T.dynExpPos; sh->fetchDstMask = T.dynDstMask; sh->dynBase = !T.memexport && T.dynFetchPos.size() <= 32 && T.dynExpPos.size() <= 64; }
    fallback = (!isVs && T.hasLoop) ? to_level0(src) : std::string();
    if (fallback == src) fallback.clear();
}
static XShader* build_shader(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen) {
    static std::unordered_map<uint64_t, XShader*> cache;
    uint64_t h = fnv64(code.data(), code.size() * 4, isVs ? 0x1234 : 0x5678);
    h = fnv64(&paramGen, 4, h);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    XShader* sh = new XShader();
    sh->hash = h;
    cache[h] = sh;
    shader_record(code, isVs, paramGen);
    shader_find(code, isVs);
    if (const char* ud = getenv("ASURA_UCODEDUMP")) {                         // the raw microcode of each new shader, big-endian as in guest memory
        std::string p = std::string(ud) + S("\\%s_%016llX.bin", isVs ? "vs" : "ps", (unsigned long long)h);
        if (FILE* f = fopen(p.c_str(), "wb")) { for (uint32_t w : code) { uint32_t be = _byteswap_ulong(w); fwrite(&be, 4, 1, f); } fclose(f); }
    }
    std::string src, fallback;
    translate_shader(code, isVs, paramGen, h, sh, src, fallback);
    static int total = 0;
    total++;
    if (total <= 3 || (total % 50) == 0) fprintf(stderr, "[gfx] shaders translated=%d (last %s %zu dwords, %zu tex, %zu vf)\n", total, isVs ? "vs" : "ps", code.size(), sh->tex.size(), sh->vf.size());
    static const bool kSync = getenv("ASURA_SYNCSHADERS") != nullptr;         // compile on this thread, as before
    auto blob = load_cached(src, isVs ? "vs_5_0" : "ps_5_0");                // already compiled once: cheap enough to do right here
    if (!blob && kSync) {
        blob = compile_cached(src, "main", isVs ? "vs_5_0" : "ps_5_0");
        if (!blob && !fallback.empty()) blob = compile_cached(fallback, "main", "ps_5_0");
    }
    if (blob || kSync) finish_shader(sh, blob.Get(), isVs);
    else queue_compile(sh, std::move(src), isVs, std::move(fallback));
    return sh;
}

// ---- compiling shaders ahead of time ----
// The game keeps its shaders in one bundle. Pixel shaders are used as they are there, so all of them can be compiled at first run. Vertex shaders are
// patched at run time to fit each mesh's vertex format, which the bundle doesn't say. So a manifest lists which shaders were used with which changed
// words, recorded by playing (BCR2_SHADERRECORD). It holds no shader code, only those patches, and is applied to the player's own game files.
const char* get_content_root();
static const char* const kShaderBundle = "3b6c2da5af6f2e42.bundle";
static std::vector<uint8_t> g_packData;
static std::vector<PackShader> g_pack;
static bool g_packLoaded = false;
static std::string g_manifestPath, g_refPath;                             // local whole-microcode record (cache\), shippable references
static std::set<std::string> g_manifestLines, g_refLines;
static bool shader_recording() { static const bool on = getenv("ASURA_NORECORD") == nullptr; return on; }   // always (ASURA_NORECORD turns it off): every shader the game uses is added to the manifest, so the next start compiles it ahead of time

static std::string exe_dir() {
    wchar_t w[MAX_PATH]; GetModuleFileNameW(nullptr, w, MAX_PATH);
    char a[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, w, -1, a, MAX_PATH, nullptr, nullptr);
    std::string s = a; return s.substr(0, s.find_last_of('\\'));
}
static uint64_t pack_key(const PackShader& p) { return fnv64(&g_packData[p.offset], p.words * 4, p.isVs ? 0x1234 : 0x5678); }
static bool pack_load() {
    if (g_packLoaded) return !g_pack.empty();
    g_packLoaded = true;
    const std::string root = get_content_root();
    if (root.empty()) return false;
    const std::string path = root + "\\assets\\" + kShaderBundle;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;     // BCR2's bundle; Asura uses the shader index instead
    if (!shaderpack_load(path, g_packData, g_pack)) { fprintf(stderr, "[shader] could not read %s\n", path.c_str()); g_pack.clear(); return false; }
    fprintf(stderr, "[shader] %zu shader containers in %s\n", g_pack.size(), kShaderBundle);
    return !g_pack.empty();
}
static void manifest_read() {
    if (const char* e = getenv("ASURA_SHADERMANIFEST")) g_manifestPath = e;
    else {                                                                   // Asura: cache\shader_manifest.txt under the data root (out of git: it holds game microcode)
        const std::string dir = bcr2_data_root() + "\\cache";
        CreateDirectoryA(dir.c_str(), nullptr);
        g_manifestPath = dir + "\\shader_manifest.txt";
    }
    auto read_lines = [](const std::string& path, std::set<std::string>& into) {
        if (FILE* f = fopen(path.c_str(), "r")) {
            static char line[1 << 20];                                       // whole-microcode lines can be long
            while (fgets(line, sizeof line, f)) { std::string s = line; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); if (!s.empty()) into.insert(s); }
            fclose(f);
        }
    };
    read_lines(g_manifestPath, g_manifestLines);
    // The manifest that ships (beside NOTES.md, in git): "vs|ps <key> <paramGen>", the key a hash of the shader's microcode.
    // No game code: the code comes from the shader index, built from the player's own game files (shaderindex.cpp).
    g_refPath = getenv("ASURA_SHADERREFS") ? getenv("ASURA_SHADERREFS") : bcr2_data_root() + "\\shader_manifest.txt";
    read_lines(g_refPath, g_refLines);
}
// A reference line names the index entry (the game's own container) plus the words the game patched when binding the shader
// (vertex shaders get their vertex fetches written). Needs the index; without it the local record keeps the shader until it is.
static bool g_indexReady = false;
static void ref_record(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen) {   // caller holds the record lock
    if (!g_indexReady) return;
    uint64_t key = shader_code_key(code.data(), code.size(), isVs);
    std::vector<uint32_t> base;
    std::string patches;
    if (!shaderindex_find(key, base)) {
        if (!shaderindex_closest(code, isVs, key, base)) return;            // not in the game files (the local record has it)
        for (size_t i = 0; i < code.size(); i++) if (code[i] != base[i]) patches += S(" %zu=%08X", i, code[i]);
    }
    const std::string line = S("%s %016llX %08X%s", isVs ? "vs" : "ps", (unsigned long long)key, paramGen, patches.c_str());
    if (!g_refLines.insert(line).second) return;
    if (FILE* f = fopen(g_refPath.c_str(), "a")) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
}
// Records a shader the game used: its container, plus the words that differ from it.
static void shader_record(const std::vector<uint32_t>& code, bool isVs, uint32_t paramGen) {
    if (!shader_recording()) return;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    if (!pack_load()) {                                                      // Asura has no shader bundle: record the whole microcode ("V"/"P" lines)
        if (g_manifestPath.empty()) manifest_read();
        ref_record(code, isVs, paramGen);
        std::string line = S("%c %08X %zu", isVs ? 'V' : 'P', paramGen, code.size());
        for (uint32_t w : code) line += S(" %08X", w);
        if (!g_manifestLines.insert(line).second) return;
        if (FILE* f = fopen(g_manifestPath.c_str(), "a")) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
        return;
    }
    const PackShader* best = nullptr; size_t bestDiff = (size_t)-1;
    for (const PackShader& p : g_pack) {
        if (p.isVs != isVs || p.words != code.size()) continue;
        const uint32_t* w = (const uint32_t*)&g_packData[p.offset];
        size_t diff = 0;
        for (size_t i = 0; i < code.size() && diff < bestDiff; i++) if (_byteswap_ulong(w[i]) != code[i]) diff++;
        if (diff < bestDiff) { bestDiff = diff; best = &p; if (diff == 0) break; }
    }
    if (!best || bestDiff > code.size() / 2) { static int nl = 0; if (nl++ < 20) fprintf(stderr, "[shader] %s of %zu words has no container\n", isVs ? "vs" : "ps", code.size()); return; }
    if (!isVs && bestDiff == 0 && paramGen == 0xFFFFFFFFu) return;           // compiled anyway
    const uint32_t* w = (const uint32_t*)&g_packData[best->offset];
    std::string line = S("%c %016llX %08X", isVs ? 'v' : 'p', (unsigned long long)pack_key(*best), paramGen);
    for (size_t i = 0; i < code.size(); i++) if (_byteswap_ulong(w[i]) != code[i]) line += S(" %zu=%08X", i, code[i]);
    if (!g_manifestLines.insert(line).second) return;
    if (FILE* f = fopen(g_manifestPath.c_str(), "a")) { fprintf(f, "%s\n", line.c_str()); fclose(f); }
}
static void show_compile_progress(int pct);   // draws the "Compiling shaders... NN%" screen (defined with the overlays, below)
// The first-start screen shows several phases as one percentage: each phase reports 0-100, mapped into [g_progLo, g_progHi].
static int g_progLo = 0, g_progHi = 100;
static bool g_progShown = false;                                              // the screen is up
static void show_phase_progress(int pct) { show_compile_progress(g_progLo + (g_progHi - g_progLo) * std::clamp(pct, 0, 100) / 100); }
// ---- generic vertex shaders ----
// The game patches every vertex shader when it binds it: the vertex fetches get the mesh's layout (destination register, stream, stride,
// offset, format, swizzle, exponent) and the interpolator exports are renumbered or blanked to link with the pixel shader. Each patched
// copy used to be its own shader, compiled the first time it was drawn (objects popped in a few frames after camera cuts). Now the
// unpatched shader from the game files (the shader index) is compiled once as a generic shader that takes those fields from vsf/vsx, and
// each patched copy is just a set of constants. Any difference the generic form can't express keeps the old per-copy shader.
// ASURA_STATICVS=1: every copy compiled on its own, as before.
static uint64_t g_vsGeneric = 0, g_vsFallback = 0;
static XShader* build_vs_base(const std::vector<uint32_t>& code) {
    static std::unordered_map<uint64_t, XShader*> cache;
    const uint64_t h = fnv64(code.data(), code.size() * 4, 0x9D7B);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    XShader* sh = new XShader();
    sh->hash = h;
    cache[h] = sh;
    std::string src, fallback;
    translate_shader(code, true, 0xFFFFFFFFu, h, sh, src, fallback, true);
    if (!sh->dynBase) return sh;                                            // memexport, or too many fetches or exports: not used as a base
    static const bool kSync = getenv("ASURA_SYNCSHADERS") != nullptr;
    auto blob = load_cached(src, "vs_5_0");
    if (!blob && kSync) blob = compile_cached(src, "main", "vs_5_0");
    if (blob || kSync) finish_shader(sh, blob.Get(), true);
    else queue_compile(sh, std::move(src), true);
    return sh;
}
// One patched copy (pc) of base code bc, as the generic base b's constants. False if the patch touches anything but the expected fields.
static bool vs_extract(const XShader* b, const std::vector<uint32_t>& bc, const std::vector<uint32_t>& pc, XShader* v, std::string& why) {
    if (bc.size() != pc.size()) { why = "length"; return false; }
    auto bits = [](uint32_t x, int lo, int n) { return (x >> lo) & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)); };
    std::vector<uint8_t> covered(pc.size(), 0);
    uint32_t prevP[3] = { 0, 0, 0 }, prevB[3] = { 0, 0, 0 };
    std::map<uint32_t, uint32_t> vfs;
    for (size_t f = 0; f < b->fetchPos.size(); f++) {
        const uint32_t pos = b->fetchPos[f];
        if (pos + 3 > pc.size()) { why = "fetch position"; return false; }
        const uint32_t* d = &pc[pos]; const uint32_t* o = &bc[pos];
        covered[pos] = covered[pos + 1] = covered[pos + 2] = 1;
        if ((d[0] & 31) != 0) { why = "fetch replaced"; return false; }
        if (!bits(d[1], 30, 1)) memcpy(prevP, d, 12);
        if (!bits(o[1], 30, 1)) memcpy(prevB, o, 12);
        const uint32_t* fp = bits(d[1], 30, 1) ? prevP : d;
        const uint32_t* fb = bits(o[1], 30, 1) ? prevB : o;
        if (bits(fp[0], 5, 6) != bits(fb[0], 5, 6) || bits(fp[0], 30, 2) != bits(fb[0], 30, 2)) { why = "fetch index register"; return false; }
        if (bits(d[1], 31, 1) != bits(o[1], 31, 1) || bits(d[2], 31, 1) != bits(o[2], 31, 1)) { why = "fetch predicate"; return false; }
        const uint32_t slot = bits(fp[0], 20, 5) * 3 + bits(fp[0], 25, 2), stride = bits(fp[2], 0, 8), rounded = bits(fp[1], 15, 1);
        const uint32_t dst = bits(d[0], 12, 6), dsw = bits(d[1], 0, 12), fmt = bits(d[1], 16, 6);
        const uint32_t flags = bits(d[1], 12, 1) | (bits(d[1], 13, 1) << 1) | (bits(d[1], 14, 1) << 2) | (rounded << 3);
        const uint32_t ex = bits(d[1], 24, 6), off = bits(d[2], 8, 23);
        if (slot >= 96) { why = "fetch slot"; return false; }
        if (!(b->fetchDstMask >> dst & 1)) { why = S("fetch destination %u: r%u -> r%u", (unsigned)f, bits(o[0], 12, 6), dst); return false; }   // only the fetches' own registers (see emit_vfetch)
        v->vsf[f * 4 + 0] = slot | (stride << 8) | (fmt << 16) | (flags << 24);
        v->vsf[f * 4 + 1] = (uint32_t)(((int32_t)(off << 9)) >> 9);          // sign-extended, in dwords
        v->vsf[f * 4 + 2] = dsw | (ex << 12) | (dst << 18);
        auto it = vfs.find(slot);
        if (it == vfs.end() || it->second < stride) vfs[slot] = stride;
    }
    static const uint32_t kBlank[3] = { 0xC8000000u, 0x00000000u, 0x02000000u };
    for (size_t e = 0; e < b->expPos.size(); e++) {
        const uint32_t pos = b->expPos[e];
        if (pos + 3 > pc.size()) { why = "export position"; return false; }
        const uint32_t* d = &pc[pos]; const uint32_t* o = &bc[pos];
        covered[pos] = covered[pos + 1] = covered[pos + 2] = 1;
        if (d[0] == o[0] && d[1] == o[1] && d[2] == o[2]) v->vsx[e] = (int32_t)bits(o[0], 0, 6);
        else if (d[0] == kBlank[0] && d[1] == kBlank[1] && d[2] == kBlank[2]) v->vsx[e] = -1;
        else if (d[1] == o[1] && d[2] == o[2] && ((d[0] ^ o[0]) & ~0x3Fu) == 0 && (d[0] & 0x3F) < 16) v->vsx[e] = (int32_t)(d[0] & 0x3F);
        else { why = "export changed"; return false; }
    }
    for (size_t i = 0; i < pc.size(); i++)
        if (pc[i] != bc[i] && !covered[i]) { why = S("word %zu", i); return false; }
    v->vf.clear();
    for (auto& kv : vfs) v->vf.push_back({ kv.first, kv.second });
    return true;
}
static XShader* vs_variant(const std::vector<uint32_t>& code) {
    static const bool kStatic = getenv("ASURA_STATICVS") != nullptr;
    const uint32_t pg = 0xFFFFFFFFu;
    if (kStatic || !g_indexReady) return build_shader(code, true, pg);
    static std::unordered_map<uint64_t, XShader*> cache;
    uint64_t h = fnv64(code.data(), code.size() * 4, 0x1234); h = fnv64(&pg, 4, h);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    XShader* res = nullptr;
    std::string why = "not in the shader index";
    uint64_t key = 0; std::vector<uint32_t> bc;
    if (shaderindex_closest(code, true, key, bc)) {
        XShader* b = build_vs_base(bc);
        if (!b->dynBase) why = "base not generic (memexport or too many fetches/exports)";
        else {
            XShader* v = new XShader();
            if (vs_extract(b, bc, code, v, why)) {
                v->hash = h; v->base = b; v->tex = b->tex; v->ok = true;
                res = v;
                shader_record(code, true, pg);                              // still listed in the manifest
                g_vsGeneric++;
            } else delete v;
        }
    }
    if (!res) {
        res = build_shader(code, true, pg);
        g_vsFallback++;
        static int nl = 0;
        if (nl++ < 40) fprintf(stderr, "[vsgen] vs %016llX compiled on its own: %s\n", (unsigned long long)h, why.c_str());
    }
    if (((g_vsGeneric + g_vsFallback) % 100) == 0) fprintf(stderr, "[vsgen] %llu generic, %llu compiled on their own\n", (unsigned long long)g_vsGeneric, (unsigned long long)g_vsFallback);
    cache[h] = res;
    return res;
}

// Pixel-shader warm-up (Asura). The shader index holds every pixel shader in the game files, and the game binds nearly all of them unchanged
// (93% of the pixel shaders first seen in episodes 4-8 had no patched words and paramGen FFFFFFFF). A shader the game used for the first time
// had to compile while its draws were left out, so objects and lighting appeared a few frames after a camera cut. These threads translate and
// compile every indexed pixel shader into the disk cache, at the lowest priority, so build_shader finds the compiled blob at first use and the
// draw goes ahead in the same frame. Nothing is registered or recorded. Done once per build and render scale (stamp file);
// ASURA_NOPSWARM=1 turns it off, ASURA_PSWARMTHREADS=n sets the thread count.
// It starts once the shaders the game is known to use (the manifest) are compiled, and runs while the game plays (a quarter of the cores, lowest
// priority). ASURA_PSWARMFG=1 runs it in the foreground first instead, behind the "Compiling shaders" screen on all cores but one (about 7 minutes
// cold on a 24-core machine).
static void start_ps_warmup() {
    if (getenv("ASURA_NOPSWARM") || getenv("ASURA_SYNCSHADERS") || shaderindex_size() == 0) return;
    WIN32_FILE_ATTRIBUTE_DATA fa{}; wchar_t ew[MAX_PATH]; GetModuleFileNameW(nullptr, ew, MAX_PATH); GetFileAttributesExW(ew, GetFileExInfoStandard, &fa);
    const std::string stampWant = S("%08lX%08lX scale=%d index=%zu vs=%d", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime, g_resScale, shaderindex_size(), getenv("ASURA_STATICVS") ? 0 : 1);
    const std::string stampPath = shader_cache_dir() + "\\pswarm.stamp";
    if (FILE* f = fopen(stampPath.c_str(), "r")) {
        char b[160] = {}; const size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
        if (stampWant == std::string(b, n)) { fprintf(stderr, "[pswarm] every indexed pixel shader is compiled already\n"); return; }
    }
    // pixel shaders as they are, and every vertex shader as its generic base (see vs_variant); vertex shaders first, they are fewer
    auto list = std::make_shared<std::vector<std::pair<bool, std::vector<uint32_t>>>>();
    static const bool kStaticVs = getenv("ASURA_STATICVS") != nullptr;
    if (!kStaticVs) shaderindex_for_each(true, [&](uint64_t, const std::vector<uint32_t>& code) { list->push_back({ true, code }); });
    shaderindex_for_each(false, [&](uint64_t, const std::vector<uint32_t>& code) { list->push_back({ false, code }); });
    const bool foreground = getenv("ASURA_PSWARMFG") != nullptr;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nThreads = getenv("ASURA_PSWARMTHREADS") ? (unsigned)std::max(1, atoi(getenv("ASURA_PSWARMTHREADS"))) : foreground ? std::max(1u, cores - 1) : std::max(1u, cores / 4);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto left = std::make_shared<std::atomic<unsigned>>(nThreads);
    auto compiled = std::make_shared<std::atomic<size_t>>(0), failed = std::make_shared<std::atomic<size_t>>(0);
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    fprintf(stderr, "[pswarm] compiling %zu indexed shaders (generic vertex shaders, then pixel shaders) into the cache on %u %s threads\n", list->size(), nThreads,
            foreground ? "foreground" : "low-priority");
    for (unsigned t = 0; t < nThreads; t++) std::thread([=] {
        SetThreadPriority(GetCurrentThread(), foreground ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST);
        t_warmThread = true;
        for (;;) {
            const size_t i = next->fetch_add(1);
            if (i >= list->size()) break;
            const bool isVs = (*list)[i].first;
            const std::vector<uint32_t>& code = (*list)[i].second;
            const uint32_t pg = 0xFFFFFFFFu;
            uint64_t h = isVs ? fnv64(code.data(), code.size() * 4, 0x9D7B) : fnv64(&pg, 4, fnv64(code.data(), code.size() * 4, 0x5678));
            XShader tmp; std::string src, fallback;
            translate_shader(code, isVs, pg, h, &tmp, src, fallback, isVs);
            if (isVs && !tmp.dynBase) continue;
            const char* target = isVs ? "vs_5_0" : "ps_5_0";
            if (load_cached(src, target)) continue;
            static const bool kTimes = getenv("ASURA_PSWARMTIMES") != nullptr;   // [pswtime] kind, ms, words, HLSL bytes: what the warm-up spends its time on
            LARGE_INTEGER c0; QueryPerformanceCounter(&c0);
            ComPtr<ID3DBlob> blob = compile_cached(src, "main", target);
            if (!blob && !fallback.empty()) blob = compile_cached(fallback, "main", target);
            (blob ? *compiled : *failed)++;
            if (kTimes) { LARGE_INTEGER c1; QueryPerformanceCounter(&c1);
                const double ms = (double)(c1.QuadPart - c0.QuadPart) * g_perfTicksToMs;
                fprintf(stderr, "[pswtime] %s %.0f %zu %zu %016llX\n", isVs ? "vs" : "ps", ms, code.size(), src.size(), (unsigned long long)h);
                if (ms > 8000) if (FILE* f = fopen((shader_cache_dir() + S("\\slow_%016llX.hlsl", (unsigned long long)h)).c_str(), "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); } }
            if ((i + 1) % 2000 == 0) fprintf(stderr, "[pswarm] %zu of %zu\n", i + 1, list->size());
        }
        if (--*left == 0) {
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            fprintf(stderr, "[pswarm] done: %zu compiled now, %zu failed, %zu were cached, %.0f s\n", compiled->load(), failed->load(),
                    list->size() - compiled->load() - failed->load(), (double)(t1.QuadPart - t0.QuadPart) * g_perfTicksToMs / 1000.0);
            if (FILE* f = fopen(stampPath.c_str(), "w")) { fputs(stampWant.c_str(), f); fclose(f); }
            *finished = true;
        }
    }).detach();
    if (!foreground) return;
    const bool showProgress = !getenv("ASURA_NOPROGRESS");
    int shown = -1;
    while (!*finished) {
        const int pct = (int)(std::min(next->load(), list->size()) * 100 / std::max<size_t>(list->size(), 1));
        if (pct != shown) {
            shown = pct;
            SetWindowTextA(g_hwnd, S("Asura's Wrath - compiling shaders (%d%%)", g_progLo + (g_progHi - g_progLo) * pct / 100).c_str());
            if (showProgress) show_phase_progress(pct);
        }
        Sleep(50);
    }
}
static void shader_precompile() {
    manifest_read();
    // The index of the player's own shaders (shaderindex.cpp): built once from the cooked packages (about 1.5 minutes), then
    // loaded on every start, since recording needs it too. Only when the game has no BCR2-style bundle.
    const std::string cacheDir = bcr2_data_root() + "\\cache";
    if (!pack_load()) {
        if (!shaderindex_load(cacheDir) && !getenv("ASURA_NOSHADERINDEX")) {
            const std::string cooked = bcr2_data_root() + "\\game\\disc\\BCGame\\CookedXbox360";
            fprintf(stderr, "[shader] building the shader index from %s\n", cooked.c_str());
            const bool showIdx = !getenv("ASURA_NOPROGRESS");
            int lastPct = -1;
            g_progLo = 0; g_progHi = 15;                                     // reading the game files: the first part of the first-start screen
            if (shaderindex_build(cooked, cacheDir, [&](int done, int total) {
                    const int pct = total ? done * 100 / total : 100;
                    if (pct == lastPct) return;
                    lastPct = pct;
                    SetWindowTextA(g_hwnd, S("Asura's Wrath - reading shaders from the game files (%d%%)", pct).c_str());
                    if (showIdx) show_phase_progress(pct);
                }))
                shaderindex_load(cacheDir);
            g_progLo = 15;                                                   // the compiles take the rest
        }
        g_indexReady = shaderindex_size() > 0;
        fprintf(stderr, "[shader] shader index: %zu shaders\n", shaderindex_size());
        if (g_indexReady) {                                                  // the local record becomes reference lines (once each)
            std::vector<uint32_t> c;
            for (const std::string& line : g_manifestLines) {
                if (line[0] != 'V' && line[0] != 'P') continue;
                unsigned pg = 0; size_t n = 0; int used = 0;
                if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2 || n == 0 || n > 65536) continue;
                c.resize(n);
                const char* p = line.c_str() + 1 + used; size_t k = 0; unsigned val = 0; int adv = 0;
                while (k < n && sscanf(p, " %x%n", &val, &adv) == 1) { c[k++] = val; p += adv; }
                if (k == n) ref_record(c, line[0] == 'V', pg);
            }
        }
    }
    // The shaders the game is known to use (the manifest) compile now, behind the "Compiling shaders" screen. Every other shader in the game files
    // is compiled afterwards in the background (start_ps_warmup) while the game plays. ASURA_PSWARMFG=1: all of them in the foreground, first.
    const bool warmFirst = getenv("ASURA_PSWARMFG") != nullptr;
    if (warmFirst) { g_progHi = 95; if (!pack_load()) start_ps_warmup(); if (g_progShown) g_progLo = 95; }
    g_progHi = 100;
    auto end_progress = [warmFirst] {                                        // the screen goes when the game starts
        if (g_progShown) {
            show_compile_progress(100); Sleep(400); show_compile_progress(-1);
            SetWindowTextA(g_hwnd, "Asura's Wrath (recomp)");
        }
        if (!warmFirst && !pack_load()) start_ps_warmup();
    };
    if (getenv("ASURA_NOPRECOMPILE") || getenv("ASURA_SYNCSHADERS")) { end_progress(); return; }
    // Done already for this build, render scale and manifest? Cached shaders load quickly on first use, so nothing more is needed.
    WIN32_FILE_ATTRIBUTE_DATA fa{}; wchar_t ew[MAX_PATH]; GetModuleFileNameW(nullptr, ew, MAX_PATH); GetFileAttributesExW(ew, GetFileExInfoStandard, &fa);
    const std::string stampWant = S("%08lX%08lX scale=%d manifest=%zu refs=%zu", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime, g_resScale, g_manifestLines.size(), g_refLines.size());
    const std::string stampPath = shader_cache_dir() + "\\precompile.stamp";
    if (FILE* f = fopen(stampPath.c_str(), "r")) {
        char b[128] = {}; const size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
        if (stampWant == std::string(b, n)) { fprintf(stderr, "[shader] already compiled ahead of time\n"); end_progress(); return; }
    }
    const bool havePack = pack_load();
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    std::unordered_map<uint64_t, const PackShader*> byKey;
    for (const PackShader& p : g_pack) byKey.emplace(pack_key(p), &p);
    std::vector<uint32_t> code;
    auto load = [&](const PackShader& p) { code.resize(p.words); const uint32_t* w = (const uint32_t*)&g_packData[p.offset]; for (uint32_t i = 0; i < p.words; i++) code[i] = _byteswap_ulong(w[i]); };
    size_t nPs = 0, nManifest = 0, nMissing = 0;
    if (havePack) for (const auto& kv : byKey) if (!kv.second->isVs) { load(*kv.second); build_shader(code, false, 0xFFFFFFFFu); nPs++; }
    if (!havePack) {
        // Asura. Local whole-microcode lines ("V"/"P", cache\) back up the index, and each becomes a reference line if it isn't one yet.
        std::unordered_map<uint64_t, std::vector<uint32_t>> local;
        for (const std::string& line : g_manifestLines) {
            if (line[0] != 'V' && line[0] != 'P') continue;
            unsigned pg = 0; size_t n = 0; int used = 0;
            if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2 || n == 0 || n > 65536) continue;
            code.resize(n);
            const char* p = line.c_str() + 1 + used; size_t k = 0; unsigned val = 0; int adv = 0;
            while (k < n && sscanf(p, " %x%n", &val, &adv) == 1) { code[k++] = val; p += adv; }
            if (k != n) continue;
            const bool vs = line[0] == 'V';
            local[shader_code_key(code.data(), n, vs)] = code;
        }
        size_t fromIndex = 0, fromLocal = 0;
        std::set<uint64_t> builtLocal;
        for (const std::string& line : g_refLines) {
            char kind[4] = {}; unsigned long long key = 0; unsigned pg = 0; int used = 0;
            if (sscanf(line.c_str(), "%2s %llx %x%n", kind, &key, &pg, &used) != 3) continue;
            const bool vs = kind[0] == 'v';
            if (shaderindex_find(key, code)) {
                fromIndex++;
                const char* p = line.c_str() + used; unsigned idx = 0, val = 0; int adv = 0;   // the words the game patched
                while (sscanf(p, " %u=%x%n", &idx, &val, &adv) == 2) { if (idx < code.size()) code[idx] = val; p += adv; }
                builtLocal.insert(shader_code_key(code.data(), code.size(), vs));
                if (vs) vs_variant(code); else build_shader(code, vs, pg);
                nManifest++;
                continue;
            }
            nMissing++;
        }
        for (auto& kv : local) if (!builtLocal.count(kv.first)) fromLocal++;  // recorded shaders the game files don't hold
        // build those from the local record, with their own paramGen
        for (const std::string& line : g_manifestLines) {
            if (line[0] != 'V' && line[0] != 'P') continue;
            unsigned pg = 0; size_t n = 0; int used = 0;
            if (sscanf(line.c_str() + 1, " %x %zu%n", &pg, &n, &used) != 2 || n == 0 || n > 65536) continue;
            code.resize(n);
            const char* p = line.c_str() + 1 + used; size_t k = 0; unsigned val = 0; int adv = 0;
            while (k < n && sscanf(p, " %x%n", &val, &adv) == 1) { code[k++] = val; p += adv; }
            if (k != n || builtLocal.count(shader_code_key(code.data(), n, line[0] == 'V'))) continue;
            if (line[0] == 'V') vs_variant(code); else build_shader(code, false, pg);
            nManifest++;
        }
        fprintf(stderr, "[shader] references: %zu, from the shader index %zu (index holds %zu), from the local record %zu, missing %zu\n",
                g_refLines.size(), fromIndex, shaderindex_size(), fromLocal, nMissing);
    }
    for (const std::string& line : g_manifestLines) {
        if (line[0] == 'V' || line[0] == 'P') continue;                      // handled above
        if (!havePack) continue;
        char kind = 0; unsigned long long key = 0; unsigned pg = 0; int used = 0;
        if (sscanf(line.c_str(), "%c %llx %x%n", &kind, &key, &pg, &used) != 3) continue;
        auto it = byKey.find(key);
        if (it == byKey.end() || it->second->isVs != (kind == 'v')) { nMissing++; continue; }
        load(*it->second);
        const char* p = line.c_str() + used; unsigned idx = 0, val = 0; int adv = 0;
        while (sscanf(p, " %u=%x%n", &idx, &val, &adv) == 2) { if (idx < code.size()) code[idx] = val; p += adv; }
        build_shader(code, kind == 'v', pg);
        nManifest++;
    }
    const int total = g_compilePending.load();
    {   // the game waits for these, so more threads help: the background workers (6) plus helpers that stop when the queue is empty
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const unsigned helpers = total > 64 && cores > 7 ? cores - 7 : 0;
        for (unsigned i = 0; i < helpers; i++) std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
                CompileJob job;
                {
                    std::lock_guard<std::mutex> lk(g_compileLock);
                    if (g_compileQueue.empty()) return;
                    job = std::move(g_compileQueue.front()); g_compileQueue.pop_front();
                }
                auto blob = compile_cached(job.src, "main", job.isVs ? "vs_5_0" : "ps_5_0");
                if (!blob && !job.fallback.empty()) blob = compile_cached(job.fallback, "main", job.isVs ? "vs_5_0" : "ps_5_0");
                finish_shader(job.sh, blob.Get(), job.isVs);
                g_compilePending--;
            }
        }).detach();
        if (helpers) fprintf(stderr, "[shader] %u extra compile threads until the queue is empty\n", helpers);
    }
    { LARGE_INTEGER tq; QueryPerformanceCounter(&tq);
      fprintf(stderr, "[shader] compiling ahead of time: %zu pixel shaders, %zu from the manifest (%zu not found), %d need compiling (queued after %.1f s)\n",
              nPs, nManifest, nMissing, total, (double)(tq.QuadPart - t0.QuadPart) * g_perfTicksToMs / 1000.0); }
    int shown = -1;
    const bool showProgress = (total > 0 || g_progShown) && !getenv("ASURA_NOPROGRESS");
    if (showProgress) show_phase_progress(0);
    while (g_compilePending.load() > 0) {
        const int left = g_compilePending.load();
        if (left != shown) {
            shown = left; SetWindowTextA(g_hwnd, S("Asura's Wrath - preparing shaders (%d of %d)", total - left, total).c_str());
            if (showProgress) show_phase_progress((total - left) * 100 / total);
        }
        Sleep(50);
    }
    if (showProgress) { show_phase_progress(100); end_progress(); }
    SetWindowTextA(g_hwnd, "Asura's Wrath (recomp)");
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[shader] done in %.1f s\n", (double)(t1.QuadPart - t0.QuadPart) * g_perfTicksToMs / 1000.0);
    if (FILE* f = fopen(stampPath.c_str(), "w")) { fputs(stampWant.c_str(), f); fclose(f); }
    if (!shader_recording()) { std::vector<uint8_t>().swap(g_packData); g_pack.clear(); g_packLoaded = false; }   // no longer needed
}

static inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---- render targets ----
struct Rt {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv, stencilSrv;
    uint32_t w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint64_t seq = 0;                                                       // write order, for colour/depth EDRAM aliasing
};
static std::unordered_map<uint64_t, Rt*> g_rts;
static uint64_t g_rtSeq = 0;

static DXGI_FORMAT rt_dxgi(uint32_t f) {
    switch (f) {
      case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
      case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
      case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 4: return DXGI_FORMAT_R16G16_FLOAT;                          // 16_16 / 16_16_16_16 hold -32..32 on the console: kept as the value
      case 5: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 6: return DXGI_FORMAT_R16G16_FLOAT;
      case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
      case 14: return DXGI_FORMAT_R32_FLOAT;
      case 15: return DXGI_FORMAT_R32G32_FLOAT;
    }
    logf_once(S("rtf%u", f).c_str(), "unknown colour target format %u", f);
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}
// Host targets are as tall as the pitch could reach inside 10 MB of EDRAM at 32 bpp, since the height is never stated.
// Also no taller than a host texture can be (16384 rows) at the render scale: the viewport is the target's full size, and at scale 4 a narrow target
// (the colour table's, pitch 80, 8192 rows) asked for a 32768-row viewport, past D3D11's limit, and its draws came out wrong (the picture turned green).
static void rt_size(uint32_t pitch, uint32_t& w, uint32_t& h) {
    w = std::max(pitch, 16u);
    h = std::clamp(align_up(2621440u / w, 8), 720u, 8192u);
    h = std::min(h, 16384u / (uint32_t)g_resScale);
}

static Rt* get_color_rt(uint32_t base, uint32_t fmt, uint32_t pitch) {
    uint64_t key = ((uint64_t)base << 32) | ((uint64_t)fmt << 16) | (pitch & 0xFFFF);
    auto it = g_rts.find(key);
    if (it != g_rts.end()) return it->second;
    Rt* r = new Rt(); rt_size(pitch, r->w, r->h); r->fmt = rt_dxgi(fmt);
    logf_once(S("rtnew%u_%u", fmt, pitch).c_str(), "colour target format %u pitch %u (host format %u) first used at frame %llu", fmt, pitch, (unsigned)r->fmt, (unsigned long long)g_frame);
    D3D11_TEXTURE2D_DESC d{}; d.Width = r->w * g_resScale; d.Height = std::min(r->h * g_resScale, 16384u); d.MipLevels = 1; d.ArraySize = 1; d.Format = r->fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &r->tex);
    g_dev->CreateRenderTargetView(r->tex.Get(), nullptr, &r->rtv);
    g_dev->CreateShaderResourceView(r->tex.Get(), nullptr, &r->srv);
    float z[4] = { 0, 0, 0, 0 }; g_ctx->ClearRenderTargetView(r->rtv.Get(), z);
    fprintf(stderr, "[gfx] colour target base=%X fmt=%u pitch=%u -> %ux%u\n", base, fmt, pitch, r->w, r->h);
    return g_rts[key] = r;
}
static Rt* get_depth_rt(uint32_t base, uint32_t pitch) {
    uint64_t key = ((uint64_t)base << 32) | (0xFFFFull << 16) | (pitch & 0xFFFF);
    auto it = g_rts.find(key);
    if (it != g_rts.end()) return it->second;
    // ASURA_DEPTH32=1 (experiment): 32-bit float depth instead of 24-bit unorm. The scene's depth is the console's 24-bit float (D24FS8), and with
    // reversed depth distant geometry sits near 0, where unorm keeps far less precision than the console.
    static const bool k32 = getenv("ASURA_DEPTH32") != nullptr;
    Rt* r = new Rt(); rt_size(pitch, r->w, r->h); r->fmt = k32 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    D3D11_TEXTURE2D_DESC d{}; d.Width = r->w * g_resScale; d.Height = std::min(r->h * g_resScale, 16384u); d.MipLevels = 1; d.ArraySize = 1; d.Format = r->fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &r->tex);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = k32 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(r->tex.Get(), &dv, &r->dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = k32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(r->tex.Get(), &sv, &r->srv);
    sv.Format = k32 ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    g_dev->CreateShaderResourceView(r->tex.Get(), &sv, &r->stencilSrv);
    g_ctx->ClearDepthStencilView(r->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    fprintf(stderr, "[gfx] depth target base=%X pitch=%u -> %ux%u\n", base, pitch, r->w, r->h);
    return g_rts[key] = r;
}

static void dump_surfaces(uint32_t only = 0, int seq = -1);
// ---- resolved surfaces, keyed by guest address ----
struct Surface {
    uint32_t base = 0, end = 0, w = 0, h = 0, pitch = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv;
    // Depth surfaces also keep the bytes guest memory would hold (D24 << 8 | S8), because shaders read depth as k_8_8_8_8.
    ComPtr<ID3D11Texture2D> rawTex; ComPtr<ID3D11RenderTargetView> rawRtv; ComPtr<ID3D11ShaderResourceView> rawSrv;
    uint64_t guestHash = 0;
    bool swapped = false;
};
static std::map<uint32_t, Surface*> g_surfaces;

static uint64_t guest_sample_hash(uint32_t addr, uint32_t size) {
    // sparse: enough to notice the guest overwriting a resolve destination with new CPU data
    uint64_t h = 0;
    uint32_t step = std::max(size / 64u, 256u) & ~7u;
    for (uint32_t o = 0; o + 8 <= size; o += step) { uint64_t v; memcpy(&v, phys(addr + o), 8); h = (h ^ v) * 0x100000001B3ull; }
    return h;
}

static Surface* make_surface(uint32_t base, uint32_t pitch, uint32_t height, uint32_t bytes, DXGI_FORMAT fmt) {
    // The game resolves one buffer in two formats every frame (B130000: float, then unorm). Retired surfaces are kept by
    // address, size and format and taken back, instead of creating textures twice a frame.
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, int>, Surface*> spare;
    auto it = g_surfaces.find(base);
    if (it != g_surfaces.end()) {
        Surface* old = it->second; g_surfaces.erase(it);
        auto k = std::make_tuple(old->base, old->pitch, old->h, old->end - old->base, (int)old->fmt);
        auto sp = spare.find(k);
        if (sp != spare.end()) { delete sp->second; sp->second = old; }
        else if (spare.size() < 64) spare[k] = old;
        else delete old;
    }
    {   auto sp = spare.find(std::make_tuple(base, pitch, height, bytes, (int)fmt));
        if (sp != spare.end()) {
            Surface* s = sp->second; spare.erase(sp);
            g_surfaces[base] = s;
            s->guestHash = guest_sample_hash(base, bytes);
            return s;
        }
    }
    Surface* s = new Surface(); s->base = base; s->end = base + bytes; s->w = pitch; s->h = height; s->pitch = pitch; s->fmt = fmt;
    D3D11_TEXTURE2D_DESC d{}; d.Width = pitch * g_resScale; d.Height = height * g_resScale; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&d, nullptr, &s->tex);
    if (!s->tex) { delete s; return nullptr; }
    g_dev->CreateRenderTargetView(s->tex.Get(), nullptr, &s->rtv);
    g_dev->CreateShaderResourceView(s->tex.Get(), nullptr, &s->srv);
    if (fmt == DXGI_FORMAT_R32_FLOAT) {
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dev->CreateTexture2D(&d, nullptr, &s->rawTex);
        g_dev->CreateRenderTargetView(s->rawTex.Get(), nullptr, &s->rawRtv);
        g_dev->CreateShaderResourceView(s->rawTex.Get(), nullptr, &s->rawSrv);
    }
    fprintf(stderr, "[gfx] resolve surface base=%X %ux%u fmt=%d\n", base, pitch, height, (int)fmt);
    g_surfaces[base] = s;
    s->guestHash = guest_sample_hash(base, bytes);
    return s;
}
static Surface* surface_at(uint32_t addr) {
    auto it = g_surfaces.upper_bound(addr);
    if (it == g_surfaces.begin()) return nullptr;
    --it;
    return addr < it->second->end ? it->second : nullptr;
}

// ---- tiling ----
static inline uint32_t tiled2d(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bpbLog2) {
    uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oib = (outer | inner) << bpbLog2;
    uint32_t bank = (y >> 4) & 1, pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1), ylsb = y & 1;
    return (ylsb << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
static inline uint32_t log2_ceil(uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; }

// ---- textures ----
struct TexFmt { uint32_t bpb, block; DXGI_FORMAT dxgi; int conv; };   // conv: 0 direct, otherwise unpack to RGBA8
static bool tex_fmt(uint32_t f, TexFmt& o) {
    switch (f) {
      case 2: case 8: case 9: case 39: o = { 1, 1, DXGI_FORMAT_R8_UNORM, 0 }; return true;
      case 3: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1 }; return true;
      case 4: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 2 }; return true;
      case 5: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 3 }; return true;
      case 6: case 14: case 50: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0 }; return true;
      case 7: case 54: o = { 4, 1, DXGI_FORMAT_R10G10B10A2_UNORM, 0 }; return true;
      case 10: case 40: o = { 2, 1, DXGI_FORMAT_R8G8_UNORM, 0 }; return true;
      case 13: case 25: o = { 4, 1, DXGI_FORMAT_R16G16_UNORM, 0 }; return true;
      case 15: o = { 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4 }; return true;
      case 16: case 55: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 5 }; return true;
      case 17: case 56: o = { 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 6 }; return true;
      case 18: case 51: o = { 8, 4, DXGI_FORMAT_BC1_UNORM, 0 }; return true;
      case 19: case 52: o = { 16, 4, DXGI_FORMAT_BC2_UNORM, 0 }; return true;
      case 20: case 53: o = { 16, 4, DXGI_FORMAT_BC3_UNORM, 0 }; return true;
      case 21: case 26: o = { 8, 1, DXGI_FORMAT_R16G16B16A16_UNORM, 0 }; return true;
      case 22: case 23: case 33: case 36: o = { 4, 1, DXGI_FORMAT_R32_FLOAT, 0 }; return true;
      case 24: o = { 2, 1, DXGI_FORMAT_R16_UNORM, 0 }; return true;
      case 27: o = { 2, 1, DXGI_FORMAT_R16_FLOAT, 0 }; return true;           // _EXPAND: 16-bit floats (as Xenia)
      case 28: o = { 4, 1, DXGI_FORMAT_R16G16_FLOAT, 0 }; return true;
      case 29: o = { 8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0 }; return true;
      case 30: o = { 2, 1, DXGI_FORMAT_R16_FLOAT, 0 }; return true;
      case 31: o = { 4, 1, DXGI_FORMAT_R16G16_FLOAT, 0 }; return true;
      case 32: o = { 8, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0 }; return true;
      case 37: o = { 8, 1, DXGI_FORMAT_R32G32_FLOAT, 0 }; return true;
      case 38: o = { 16, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0 }; return true;
      case 49: o = { 16, 4, DXGI_FORMAT_BC5_UNORM, 0 }; return true;
      case 59: o = { 8, 4, DXGI_FORMAT_BC4_UNORM, 0 }; return true;
    }
    return false;
}
static inline void swap_block(uint8_t* p, uint32_t n, uint32_t endian) {
    switch (endian) {
      case 1: for (uint32_t i = 0; i + 2 <= n; i += 2) std::swap(p[i], p[i + 1]); break;
      case 2: for (uint32_t i = 0; i + 4 <= n; i += 4) *(uint32_t*)(p + i) = _byteswap_ulong(*(uint32_t*)(p + i)); break;
      case 3: for (uint32_t i = 0; i + 4 <= n; i += 4) { uint32_t v = *(uint32_t*)(p + i); *(uint32_t*)(p + i) = (v >> 16) | (v << 16); } break;
    }
}
static void unpack_rgba8(int conv, const uint8_t* s, uint8_t* d) {
    auto s5 = [](uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); };
    auto s6 = [](uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); };
    auto s4 = [](uint32_t v) { return (uint8_t)((v << 4) | v); };
    uint32_t v = conv >= 5 ? *(const uint32_t*)s : *(const uint16_t*)s;
    switch (conv) {
      case 1: d[0] = s5(v & 31); d[1] = s5((v >> 5) & 31); d[2] = s5((v >> 10) & 31); d[3] = (v >> 15) ? 255 : 0; break;
      case 2: d[0] = s5(v & 31); d[1] = s6((v >> 5) & 63); d[2] = s5((v >> 11) & 31); d[3] = 255; break;
      case 3: d[0] = s6(v & 63); d[1] = s5((v >> 6) & 31); d[2] = s5((v >> 11) & 31); d[3] = 255; break;
      case 4: d[0] = s4(v & 15); d[1] = s4((v >> 4) & 15); d[2] = s4((v >> 8) & 15); d[3] = s4((v >> 12) & 15); break;
      case 5: d[0] = (uint8_t)((v & 0x7FF) >> 3); d[1] = (uint8_t)(((v >> 11) & 0x7FF) >> 3); d[2] = (uint8_t)((v >> 22) >> 2); d[3] = 255; break;
      case 6: d[0] = (uint8_t)((v & 0x3FF) >> 2); d[1] = (uint8_t)(((v >> 10) & 0x7FF) >> 3); d[2] = (uint8_t)((v >> 21) >> 3); d[3] = 255; break;
    }
}

struct HostTex {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srvSigned;                             // the same texels read as signed (SNORM), made on first use
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN; uint32_t faces = 1;
    uint64_t hash = 0, lastFrame = ~0ull, mxEpoch = 0; uint32_t hostW = 0, hostH = 0, levels = 0;
    bool white = false;                                                     // DXT5 that is white wherever it is not transparent (font pages)
    HostTex* mod = nullptr;                                                 // a replacement from the mods folder for the current contents
};
// Signed textures (normal maps above all) were read as unsigned and turned signed in texfix after filtering. Byte 7F (+1) and 80 (-1) are neighbours
// in unsigned order, so filtering across zero gave values near -1: bumpy metal and clouds sparkled at a distance. Formats with an SNORM twin are
// created typeless, and a fetch that reads every channel as signed gets the SNORM view: the GPU filters signed values. ASURA_NOSNORM=1 for the old way.
static DXGI_FORMAT snorm_twin(DXGI_FORMAT f, DXGI_FORMAT* typeless, int* channels) {
    switch (f) {
      case DXGI_FORMAT_R8_UNORM:       *typeless = DXGI_FORMAT_R8_TYPELESS;       *channels = 1; return DXGI_FORMAT_R8_SNORM;
      case DXGI_FORMAT_R8G8_UNORM:     *typeless = DXGI_FORMAT_R8G8_TYPELESS;     *channels = 2; return DXGI_FORMAT_R8G8_SNORM;
      case DXGI_FORMAT_R8G8B8A8_UNORM: *typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; *channels = 4; return DXGI_FORMAT_R8G8B8A8_SNORM;
      case DXGI_FORMAT_BC4_UNORM:      *typeless = DXGI_FORMAT_BC4_TYPELESS;      *channels = 1; return DXGI_FORMAT_BC4_SNORM;
      case DXGI_FORMAT_BC5_UNORM:      *typeless = DXGI_FORMAT_BC5_TYPELESS;      *channels = 2; return DXGI_FORMAT_BC5_SNORM;
      default: return DXGI_FORMAT_UNKNOWN;
    }
}
static ComPtr<ID3D11ShaderResourceView> make_srv(ID3D11Texture2D* tex, DXGI_FORMAT f, uint32_t faces) {
    D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = f;
    if (faces == 6) { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; v.TextureCube.MipLevels = (UINT)-1; }
    else { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = (UINT)-1; }
    ComPtr<ID3D11ShaderResourceView> s; g_dev->CreateShaderResourceView(tex, &v, &s);
    return s;
}
static std::unordered_map<uint64_t, HostTex*> g_texCache;

static uint64_t range_hash(uint32_t addr, uint32_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t o = 0; o + 8 <= size; o += 32) { uint64_t v; memcpy(&v, phys(addr + o), 8); h = (h ^ v) * 0x100000001B3ull; }
    return h;
}

struct FetchInfo { uint32_t base, fmt, w, h, pitch, endian, dim, faces, mipBase, maxLevel; bool tiled, packed; };

// ---- mip chain layout, as the console stores it (same rules as Xenia's texture_util.cc) ----
// Levels 1 and up live at the fetch constant's mip address. Each stores a power-of-two-based pitch and height, tile-aligned, on a 4 KB boundary.
// Once the short side reaches 16 texels, every smaller level shares one tile (the packed tail), each at its own offset.
struct MipStore { uint32_t offset = 0, pitchBlocks = 0, rowBytes = 0, stride = 0; };
static uint32_t next_pow2(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }
static uint32_t packed_mip_level(uint32_t w, uint32_t h) {
    uint32_t l = log2_ceil(std::min(w, h));
    return l <= 4 ? 0 : l - 4;
}
static void packed_mip_offset(uint32_t w, uint32_t h, uint32_t level, uint32_t block, uint32_t& x, uint32_t& y) {
    uint32_t lw = log2_ceil(w), lh = log2_ceil(h), ls = std::min(lw, lh);
    uint32_t base = ls > 4 ? ls - 4 : 0, m = level - base;
    x = y = 0;
    if (m < 3) {
        if (lw > lh) y = 16 >> m; else x = 16 >> m;
    } else {
        if (lw > lh) x = (1u << (lw - base)) >> (m - 2); else y = (1u << (lh - base)) >> (m - 2);
    }
    x /= block; y /= block;
}

// BCR2_TEXDUMP=<dir>: every texture of 64 pixels or more is written as two BMPs the first time it is uploaded and whenever its content changes: <base>_f<fmt>_<w>x<h>_rgb.bmp
// and _a.bmp (alpha as grey). Compressed formats are decoded here on the CPU. For finding what an unknown texture is, such as a font atlas.
static void bc_colors(const uint8_t* b, uint8_t c[4][4], bool bc1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    auto ex = [](uint16_t v, uint8_t* o) { o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255; };
    ex(c0, c[0]); ex(c1, c[1]);
    if (!bc1 || c0 > c1) { for (int i = 0; i < 3; i++) { c[2][i] = (uint8_t)((2 * c[0][i] + c[1][i]) / 3); c[3][i] = (uint8_t)((c[0][i] + 2 * c[1][i]) / 3); } c[2][3] = c[3][3] = 255; }
    else { for (int i = 0; i < 3; i++) { c[2][i] = (uint8_t)((c[0][i] + c[1][i]) / 2); c[3][i] = 0; } c[2][3] = 255; c[3][3] = 0; }
}
static void write_bmp(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, bool alphaOnly) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t imgBytes = w * h * 4, off = 54;
    uint8_t hd[54] = { 'B', 'M' };
    auto p32 = [&](int at, uint32_t v) { memcpy(hd + at, &v, 4); };
    p32(2, off + imgBytes); p32(10, off); p32(14, 40); p32(18, w); p32(22, (uint32_t)(-(int32_t)h)); hd[26] = 1; hd[28] = 32; p32(34, imgBytes);
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = &rgba[((size_t)y * w + x) * 4];
            uint8_t* d = &row[x * 4];
            if (alphaOnly) d[0] = d[1] = d[2] = s[3]; else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
            d[3] = alphaOnly ? 255 : s[3];   // colour dumps keep their alpha (tools\bmp2png.py --alpha makes a transparent PNG)
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}
// The host texture data of one level (as get_texture builds it) as 8-bit RGBA, for the dumps: false for formats it cannot show.
// R8 and BC4 come out as grey with full alpha, R8G8 and BC5 as red and green.
static bool decode_rgba(const TexFmt& tf, uint32_t w, uint32_t h, const uint8_t* data, std::vector<uint8_t>& rgba) {
    rgba.assign((size_t)w * h * 4, 0);
    auto alpha_block = [](const uint8_t* b, uint8_t al[16]) {
        uint8_t a0 = b[0], a1 = b[1], tab[8]; tab[0] = a0; tab[1] = a1;
        if (a0 > a1) for (int i = 1; i < 7; i++) tab[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        else { for (int i = 1; i < 5; i++) tab[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); tab[6] = 0; tab[7] = 255; }
        uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) al[i] = tab[(bits >> (3 * i)) & 7];
    };
    if (tf.conv || tf.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM) { memcpy(rgba.data(), data, rgba.size()); return true; }
    if (tf.dxgi == DXGI_FORMAT_R8_UNORM || tf.dxgi == DXGI_FORMAT_R8G8_UNORM) {
        const bool two = tf.dxgi == DXGI_FORMAT_R8G8_UNORM;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t* d = &rgba[i * 4];
            if (two) { d[0] = data[i * 2]; d[1] = data[i * 2 + 1]; } else d[0] = d[1] = d[2] = data[i];
            d[3] = 255;
        }
        return true;
    }
    if (tf.dxgi == DXGI_FORMAT_BC4_UNORM || tf.dxgi == DXGI_FORMAT_BC5_UNORM) {
        const bool two = tf.dxgi == DXGI_FORMAT_BC5_UNORM;
        const uint32_t bw = w / 4, bh = h / 4;
        for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = data + ((size_t)by * bw + bx) * tf.bpb;
            uint8_t r[16], g[16]; alpha_block(b, r); if (two) alpha_block(b + 8, g);
            for (int i = 0; i < 16; i++) {
                uint8_t* d = &rgba[(((size_t)by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4];
                if (two) { d[0] = r[i]; d[1] = g[i]; } else d[0] = d[1] = d[2] = r[i];
                d[3] = 255;
            }
        }
        return true;
    }
    if (tf.dxgi == DXGI_FORMAT_BC1_UNORM || tf.dxgi == DXGI_FORMAT_BC2_UNORM || tf.dxgi == DXGI_FORMAT_BC3_UNORM) {
        const bool bc1 = tf.dxgi == DXGI_FORMAT_BC1_UNORM, bc3 = tf.dxgi == DXGI_FORMAT_BC3_UNORM;
        const uint32_t bw = w / 4, bh = h / 4;
        for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = data + ((size_t)by * bw + bx) * tf.bpb;
            uint8_t c[4][4]; bc_colors(bc1 ? b : b + 8, c, bc1);
            uint8_t al[16]; for (int i = 0; i < 16; i++) al[i] = 255;
            if (tf.dxgi == DXGI_FORMAT_BC2_UNORM) { for (int i = 0; i < 16; i++) { uint8_t n = (uint8_t)((b[i / 2] >> ((i & 1) * 4)) & 15); al[i] = (uint8_t)(n * 17); } }
            if (bc3) {
                uint8_t a0 = b[0], a1 = b[1], tab[8]; tab[0] = a0; tab[1] = a1;
                if (a0 > a1) for (int i = 1; i < 7; i++) tab[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                else { for (int i = 1; i < 5; i++) tab[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); tab[6] = 0; tab[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) al[i] = tab[(bits >> (3 * i)) & 7];
            }
            uint32_t ci = (uint32_t)((bc1 ? b : b + 8)[4] | (bc1 ? b : b + 8)[5] << 8 | (bc1 ? b : b + 8)[6] << 16 | (uint32_t)(bc1 ? b : b + 8)[7] << 24);
            for (int i = 0; i < 16; i++) {
                const uint8_t* cc = c[(ci >> (2 * i)) & 3];
                uint8_t* d = &rgba[(((size_t)by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4];
                d[0] = cc[0]; d[1] = cc[1]; d[2] = cc[2]; d[3] = (bc1 && cc[3] == 0) ? 0 : (tf.dxgi == DXGI_FORMAT_BC1_UNORM ? 255 : al[i]);
            }
        }
        return true;
    }
    return false;
}
static void dump_texture_bmp(const char* dir, uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, const TexFmt& tf, const uint8_t* data) {
    static std::unordered_map<uint32_t, uint64_t> last;
    static int count = 0;
    if (w < 8 && h < 8) return;
    uint64_t hs = fnv64(data, (size_t)w * h / (tf.conv ? 1 : (tf.block * tf.block)) * (tf.conv ? 4 : tf.bpb));
    auto it = last.find(base);
    if (it != last.end() && it->second == hs) return;
    last[base] = hs;
    if (count >= 8000) return;
    std::vector<uint8_t> rgba;
    if (!decode_rgba(tf, w, h, data, rgba)) return;                       // (movie planes would use up the count)
    count++;
    std::string stem = std::string(dir) + "\\" + S("%08X_f%u_%ux%u", base, fmt, w, h);
    write_bmp(stem + "_rgb.bmp", rgba, w, h, false);
    write_bmp(stem + "_a.bmp", rgba, w, h, true);
    fprintf(stderr, "[texdump] f=%llu %08X fmt=%u %ux%u written\n", (unsigned long long)g_frame, base, fmt, w, h);
}
// ---- button prompts: the game's button icons are one 256x256 RGBA atlas (LT, RT, LB, RB, back, start, sticks, A, B, X, Y). The PC Settings menu can swap
// it for art baked from another controller set (tools/bake_buttons.ps1): a 768x768 PNG laid out so the game's own glyph rectangles still land on each icon.
// The texture is recognised by a hash of its guest memory, since its address changes between levels.
static const uint64_t kButtonAtlasHash = 0xFF5B2AECE0D5A93Aull;               // the atlas as it is in the game's files (the [buttons] log line prints this)
static std::atomic<int> g_buttonSet{-1};                                      // -1 not read yet, 0 the game's own art, 1 xbox, 2 playstation, 3 switch
int settings_button_set();
void gpu_set_button_set(int s) { g_buttonSet = s < 0 ? 0 : s > 3 ? 3 : s; }
int gpu_button_set() { int s = g_buttonSet.load(); if (s < 0) { s = settings_button_set(); g_buttonSet = s; } return s; }
static std::unordered_set<uint64_t> g_buttonKeys;
static HostTex g_buttonTex[4];
static bool g_buttonFailed[4] = {};
static bool load_png_rgba(const std::wstring& path, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);                            // already initialised in another mode is fine
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> fr; ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    if (FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&cv))) return false;
    if (FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT uw = 0, uh = 0; cv->GetSize(&uw, &uh);
    w = uw; h = uh; rgba.resize((size_t)w * h * 4);
    return SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
}
static std::wstring assets_dir() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"ASURA_ASSETS", env, MAX_PATH)) return env;      // a checkout's host\assets, for builds that sit somewhere else
    wchar_t w[MAX_PATH]; GetModuleFileNameW(nullptr, w, MAX_PATH);
    const std::string a = bcr2_data_root() + "\\assets"; return std::wstring(a.begin(), a.end());
}
// The replacement for the atlas, or null to use the game's own.
// Pictures drawn over the frame after the final pass (the key and mouse icons on the Controls screen), in the 1280x720 space of the game's menus.
static std::mutex g_spriteLock;
static std::vector<GpuSprite> g_sprites;
void gpu_set_sprites(const std::vector<GpuSprite>& s) { std::lock_guard<std::mutex> lk(g_spriteLock); g_sprites = s; }
static ComPtr<ID3D11PixelShader> g_psSprite;
static ComPtr<ID3D11BlendState> g_blendAlpha;
static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> g_spriteTex;
static ID3D11ShaderResourceView* sprite_srv(const std::string& icon) {
    auto it = g_spriteTex.find(icon);
    if (it != g_spriteTex.end()) return it->second.Get();
    ComPtr<ID3D11ShaderResourceView> srv;
    std::vector<uint8_t> px; uint32_t w = 0, h = 0;
    std::wstring path = assets_dir() + L"\\keys\\" + std::wstring(icon.begin(), icon.end()) + L".png";
    if (load_png_rgba(path, px, w, h) && w && h) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex))) g_dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    } else fprintf(stderr, "[keys] no icon %ls\n", path.c_str());
    g_spriteTex[icon] = srv;
    return srv.Get();
}
static void draw_sprites(int px, int py, int pw, int ph);
// The sample picture for the gamma screen: assets\gamma.png, 1280x720. Without the file the screen shows only its slider.
static ID3D11ShaderResourceView* gamma_preview_srv() {
    static ComPtr<ID3D11Texture2D> tex; static ComPtr<ID3D11ShaderResourceView> srv; static bool tried = false;
    if (!tried) {
        tried = true;
        std::vector<uint8_t> px; uint32_t w = 0, h = 0;
        const std::wstring path = assets_dir() + L"\\gamma.png";
        if (load_png_rgba(path, px, w, h) && w && h) {
            D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
            if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex))) g_dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        }
        fprintf(stderr, "[gamma] sample picture %ls: %s\n", path.c_str(), srv ? "loaded" : "not found");
    }
    return srv.Get();
}
static HostTex* button_override() {
    const int s = gpu_button_set();
    if (s <= 0 || s > 3 || g_buttonFailed[s]) return nullptr;
    HostTex& t = g_buttonTex[s];
    if (t.srv) return &t;
    static const wchar_t* names[4] = { L"", L"xbox", L"playstation", L"switch" };
    std::vector<uint8_t> px; uint32_t w = 0, h = 0;
    const std::wstring path = assets_dir() + L"\\buttons\\" + names[s] + L".png";
    if (!load_png_rgba(path, px, w, h) || !w || !h) { g_buttonFailed[s] = true; fprintf(stderr, "[buttons] could not load %ls\n", path.c_str()); return nullptr; }
    // The game stores this atlas with red and blue the other way round and its shader swaps them back, so the same order goes in here.
    for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t.tex))) { g_buttonFailed[s] = true; return nullptr; }
    g_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    g_ctx->UpdateSubresource(t.tex.Get(), 0, nullptr, px.data(), w * 4, 0);
    g_ctx->GenerateMips(t.srv.Get());
    t.hostW = w; t.hostH = h; t.levels = 0;
    fprintf(stderr, "[buttons] set %d loaded from %ls (%ux%u)\n", s, path.c_str(), w, h);
    return &t;
}
// ---- the third start-up screen (the save warning) is a 1024x256 RGBA texture. assets\splash\save_warning_color.png and save_warning_alpha.png (red, green and blue from the
// first, alpha from the red channel of the second) replace it, so it can say PC instead of Xbox 360. Recognised by a hash of its guest memory, like the button atlas.
static const uint64_t kSplashHash = 0x12CF43706632E13Full;                                        // the [splash] log line prints the real value
static void splash_override(std::vector<uint8_t>& out) {
    static bool tried = false, ok = false;
    static std::vector<uint8_t> c, a;
    if (!tried) {
        tried = true;
        uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        const std::wstring dir = assets_dir() + L"\\splash\\";
        ok = load_png_rgba(dir + L"save_warning_color.png", c, w1, h1) && load_png_rgba(dir + L"save_warning_alpha.png", a, w2, h2) && w1 == 1024 && h1 == 256 && w2 == 1024 && h2 == 256;
        fprintf(stderr, "[splash] %s the replacement save warning from %ls\n", ok ? "loaded" : "could not load", dir.c_str());
    }
    if (!ok || out.size() != (size_t)1024 * 256 * 4) return;
    for (size_t i = 0; i < (size_t)1024 * 256; i++) { out[i * 4] = c[i * 4]; out[i * 4 + 1] = c[i * 4 + 1]; out[i * 4 + 2] = c[i * 4 + 2]; out[i * 4 + 3] = a[i * 4]; }
}
// ASURA_TEXDUMPONLY=addr,addr,... (hex): ASURA_TEXDUMP and ASURA_TEXDUMPMIPS save only textures at these addresses
// ---- QTE prompts (ASURA_AUTOQTE) ----
// A QTE prompt is a handful of UI draws whose textures are fixed for each kind of prompt (the button icon, the crest, the ring). A texture
// in the table below marks the prompt it belongs to while it is drawn; at each swap the frame's set is published, and xam.cpp adds the
// answer to the pad state (qte_active). Hashes are the texture content hash as draws_<frame>.txt shows it (64-bit hash folded to 32).
enum : uint32_t { QTE_A = 1, QTE_B = 2, QTE_X = 4, QTE_Y = 8, QTE_LT = 16, QTE_RT = 32, QTE_LS = 64, QTE_RS = 128, QTE_ROTATE = 256, QTE_YC = 512 /* the combat Y medallion: Story Mode presses it (xam.cpp) */ };
// LS/RS without the rotate arrows: push that stick down (episode 1's "push both sticks down"). With them: circle the stick.
// The UI textures sit at fixed addresses; LS and RS also share a content hash (the hash samples one dword in eight), so both are matched.
struct QteTex { uint32_t addr, hash, action; const char* name; };
static const QteTex kQteTex[] = {                                           // from the episode 1 captures (logs\qrep3)
    { 0x01496000, 0xD3B9EB89, QTE_B, "B" }, { 0x014CA000, 0x7358E170, QTE_B, "B" }, { 0x014DA000, 0xA0467C6B, QTE_B, "B" }, { 0x014EE000, 0xA799C1ED, QTE_B, "B" },
    { 0x01466000, 0, QTE_B, "B" },
    { 0x01476000, 0xCB16A818, QTE_RT, "RT" }, { 0x0147A000, 0x78A994A6, QTE_RT, "RT" }, { 0x01482000, 0xE158670C, QTE_RT, "RT" },
    { 0x014AE000, 0x46BBB154, QTE_RT, "RT" }, { 0x014C2000, 0x74048470, QTE_RT, "RT" },
    { 0x014A6000, 0x9DB98EDC, QTE_LT, "LT" }, { 0x014B6000, 0x1CF2227D, QTE_LT, "LT" },
    { 0x01486000, 0x6957789E, QTE_LS, "LS" }, { 0x0148E000, 0x0EC6D4B7, QTE_LS, "LS" },
    { 0x0148A000, 0x6957789E, QTE_RS, "RS" }, { 0x01492000, 0x0EC6D4B7, QTE_RS, "RS" },
    { 0x01456000, 0x091CD8EE, QTE_ROTATE, "rotate" },
    // the Y icon: combat prompts, QTEs and the Synchronic tracks (the same texture in every capture: f9_11011, f9_15622, f9_19504).
    // Address and hash both: matched by address only, it never hides a draw (below) and xam.cpp answers it with ASURA_AUTOQTE only.
    { 0x014FA000, 0xDF3D91A4, QTE_Y, "Y" },
    // the combat Y prompt (a gold medallion over a stunned enemy): its Y icon, 128x128 DXT5 at 014D2000, dump 423147C3DFD0039C
    // (the user's episode 16 run, logs\rec_20261122_105305 [mashtex] at f=3248, on screen 3255-3268; answered in rec_20261122_105600)
    { 0x014D2000, 0x557DF1F3, QTE_Y | QTE_YC, "Y" },
};
static std::atomic<uint32_t> g_qteActive{0};
static uint32_t g_qteFrame = 0;
// ASURA_BURSTAUTO=1 (with ASURA_QTESKIP, which never opens the QTE prompts: uescript.cpp) hides the BURST prompt of the gameplay
// sections (RT at a full Mantra gauge, drawn by the HUD) and xam.cpp presses RT for it. Off by default: BURST can also come up in
// gameplay, so Story Mode leaves it to the player (the user's call, 2026-11-12). The prompt's textures move between runs
// (the RT icons stay, the "BURST" lettering was at 02BD4000, 02BD5000 or 02BD6000, the flash at 00791000 or 0079F000; logs\burst4-7), so
// in this mode an icon is also recognised by its content hash alone, and once an icon has been drawn in the frame, the UI pieces after
// it shaped like the prompt's (fmt 20, 128x128, 512x128 or 1024x1024) are left out too. No QTE prompt is drawn in this mode, so an icon
// on screen can only be a BURST prompt.
static const bool kHidePrompts = [] { const char* e = getenv("ASURA_BURSTAUTO"); return e && *e && strcmp(e, "0") != 0; }();
static bool g_hideDraw = false;
static bool g_bossFrame = false, g_bossPrev = false;                        // episode 22's boss prompt drawn in this / the previous frame
static std::atomic<uint64_t> g_bossMs{0};                                   // when one was last drawn (GetTickCount64)
static std::atomic<uint64_t> g_combatMs{0};                                 // when a combat Y/B medallion was last drawn
bool combat_prompt_recent() { const uint64_t t = g_combatMs.load(); return t && GetTickCount64() - t < 3000; }
bool boss_prompt_recent() { const uint64_t t = g_bossMs.load(); return t && GetTickCount64() - t < 3000; }   // xma.cpp: the chime's mute
static bool g_mashBurst = false;                                            // a tug of war has shown its RT BURST prompt (qte_note_texture)
static void qte_note_texture(uint32_t base, uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h2, bool ui, bool white) {
    const uint32_t h = (uint32_t)(hash ^ (hash >> 32));
    {   // each pixel texture's folded hash once while tracing, and any texture at a prompt address whose hash is not the table's
        static std::set<uint64_t> seen;
        const bool atPrompt = std::any_of(std::begin(kQteTex), std::end(kQteTex), [&](const QteTex& q) { return q.addr == base && q.hash && q.hash != h; });
        if ((g_trace > 0 || atPrompt) && seen.insert((uint64_t)base << 32 | h).second) fprintf(stderr, "[qtetex] f=%llu base=%08X hash=%08X%s\n", (unsigned long long)g_frame, base, h, atPrompt ? " (prompt address, other hash)" : "");
    }
    bool story_mash_active();                                               // Story Mode answering a tug-of-war prompt (uescript.cpp): hide its icons
    const bool hideMash = story_mash_active();
    {   // ASURA_MASHTEXLOG=1 (debug): each fmt-20 (DXT5, the UI's format) texture once outside a tug of war and once during one, to
        // tell the tug-of-war meter's textures from the HUD's
        static const bool kLog = getenv("ASURA_MASHTEXLOG") != nullptr;
        static std::set<uint64_t> seenOut, seenIn;
        if (kLog && fmt == 20 && (hideMash ? seenIn : seenOut).insert(hash).second)
            fprintf(stderr, "[mashtex] f=%llu %s hash=%016llX base=%08X %ux%u%s%s%s\n", (unsigned long long)g_frame, hideMash ? "during" : "outside", (unsigned long long)hash, base, w, h2,
                    hideMash && !seenOut.count(hash) ? " (only seen during)" : "", white ? " white" : "", ui ? " ui" : "");
    }
    // A tug of war that ends in an RT BURST (episode 11's rotate meter): Story Mode leaves RT to the player (xam.cpp qte_apply), so from
    // the first RT icon on, nothing of the tug of war is hidden any more and the BURST prompt shows; until it has gone (story_mash_recent).
    bool story_mash_recent();
    bool& mashBurst = g_mashBurst;
    const bool mashRecent = story_mash_recent();
    if (!mashRecent) mashBurst = false;
    for (const QteTex& q : kQteTex)
        if ((q.addr == base && (q.hash == 0 || q.hash == h)) || ((kHidePrompts || hideMash) && !(q.action & QTE_Y) && q.hash && q.hash == h)) {
            if (q.action & QTE_Y) { g_qteFrame |= q.action; continue; }       // noted for xam.cpp, never hidden here (the Y icon's own rule is below)
            g_qteFrame |= q.action;
            if (q.action == QTE_RT && mashRecent && !kHidePrompts && !mashBurst) { mashBurst = true; fprintf(stderr, "[qte] f=%llu tug of war: RT BURST prompt, shown and left to the player\n", (unsigned long long)g_frame); }
            if (kHidePrompts || (hideMash && !mashBurst)) g_hideDraw = true;
        }
    if (kHidePrompts && (g_qteFrame & ~(QTE_Y | QTE_YC)) && fmt == 20 && ((w == 128 && h2 == 128) || (w == 512 && h2 == 128) || (w == 1024 && h2 == 1024))) g_hideDraw = true;
    // Story Mode answering a tug of war (uescript.cpp story_mash_recent): its meter is the HUD's own gauge (the action feeds the Mantra
    // gauge: bNotSynchronizeMantraGauge false, AdditionalMantraAmount 0.02), drawn by Scaleform from DXT5 (fmt 20) pictures: the gold
    // ornament sheet 512x512, fire sheets 512x256 / 128x256 / 1024x512, the prompt's B states, emblem, circle and glow at 128x128 / 64x64,
    // the bar burning away at the end 2048x1024 and 256x256 (episode 5, logs\ep5_run7 / st5b [mashtex]). Their content moves between
    // runs, so every Scaleform DXT5 picture is left out while it lasts except font pages (white wherever not transparent: HostTex::white),
    // so subtitles stay. Going by size alone missed the 256x256 burn sheets (the user's episode 11 run, rec_20261115_183431_2.mp4 0:59).
    if (ui && fmt == 20 && mashRecent && !mashBurst && !white) g_hideDraw = true;
    // The same while Story Mode answers episode 22's X/B choice (uescript.cpp story_choice_active): its medallions, buttons and circles
    // go, subtitles stay (the user, 2026-11-22)
    bool story_choice_active();
    if (ui && fmt == 20 && !white && story_choice_active()) g_hideDraw = true;
    {   // its own pictures whatever shader draws them, while it is open and for 1 s after: the B/X icons, ornaments, circles and the press
        // flash still showed (the red B at f=26791 and 26876, logs\rec_20261122_133826; textures from the X/B capture f9_22996)
        bool story_choice_recent1s();
        static const uint32_t kChoice[] = { 0x7358E170u, 0x557DF1F3u, 0xA0467C6Bu, 0xC616B372u, 0x02CB6CDCu, 0x9DA45AA1u, 0x4E25A304u, 0xF741C256u };
        // and always in episode 22's Story Mode, where Story Mode answers every medallion of this kind (the Y/B ones of its boss
        // fight included, uescript.cpp)
        bool story_qteskip(); bool map_is_ep22();
        if (std::find(std::begin(kChoice), std::end(kChoice), h) != std::end(kChoice) && (story_choice_recent1s() || (story_qteskip() && map_is_ep22()))) g_hideDraw = true;
    }
    // And for 1.5 s after one of episode 22's boss prompts: the boss's own QTEs end in a success or failure emblem (the black splatter at
    // f=28945, the user's run logs\rec_20261122_133826), drawn once the medallion has gone. Subtitles (font pages) stay.
    {
        bool story_qteskip();
        const uint64_t bt = g_bossMs.load();
        if (ui && fmt == 20 && !white && bt && GetTickCount64() - bt < 1500 && story_qteskip()) g_hideDraw = true;
    }
    // Story Mode with its prompts hidden (hooks.cpp synchronic_hide_enabled): the Y prompt icon (128x128 DXT5 at 014FA000 among the QTE
    // prompt icons, folded content hash DF3D91A4) is left out. A Synchronic track shows it before Story Mode can hide the track's
    // prompt, and nothing that runs afterwards takes it off (it stayed for the rest of the scene: the user's episode 3 run,
    // logs\rec_20261113_142416_2.mp4 at 0:46). Every other Y prompt is a QTE's, and Story Mode answers those without a prompt anyway.
    bool synchronic_hide_enabled();
    if (h == 0xDF3D91A4u && synchronic_hide_enabled()) g_hideDraw = true;
    // Story Mode: episode 22's boss prompts (a red Devanagari letter on an ornate medallion, for QTEs Story Mode passes anyway) are left
    // out. Their Scaleform pictures by content hash, all first drawn at the first boss prompt (the user's run, logs\rec_20261122_112050
    // [mashtex] f=21761-21935, marks 1-6): the ornament, its black backing and glow (256x256), the letters (96x100, 120x120) and the
    // lotus (128x44). While one was drawn in the previous frame, the faint button circles around it (128x128, shared with the combat
    // prompts, so only then) go too. ASURA_BOSSHIDE=0 keeps them.
    {
        static const bool kBossHide = [] { const char* e = getenv("ASURA_BOSSHIDE"); return !e || atoi(e) != 0; }();
        static const uint64_t kBoss[] = { 0x01B121DCE79B255Dull, 0x16FBC54B37D163B1ull, 0x70415191AE185B7Eull, 0xA8A3DDBF28B73879ull,
                                          0xA995243BAA6A564Full, 0xC438BFCB45354102ull, 0xA5478B82E1D0ED9Aull };
        bool story_qteskip(); bool map_is_ep22();
        // In episode 22's map the pieces beside the medallion are left out from its first frame too: going by the previous frame alone
        // let a red burst through on each one's first frame (f=29051, logs\rec_20261122_140702)
        const bool inEp22 = map_is_ep22();
        if (kBossHide && ui && story_qteskip()) {
            if (std::find(std::begin(kBoss), std::end(kBoss), hash) != std::end(kBoss)) { g_bossFrame = true; g_hideDraw = true; }
            // with them: the button circles, the gray flame (1024x512 sheet) and the flash (1024x1024, the BURST prompt's too); seen
            // left over beside the hidden medallion at f=22868 (the user's run, logs\rec_20261122_122628)
            else if ((g_bossPrev || inEp22) && (hash == 0xCC6068FA8245CBFEull || hash == 0xF38A8EE504CB4CB3ull || hash == 0xE9455A62A1BBE4B3ull || hash == 0x16799D79086AAB4Bull)) g_hideDraw = true;
            // and the boss's mash prompt's effects (the burst at each press and its rings, folded hashes as draws_<frame>.txt shows them:
            // the user's capture f9_30578, prompt on screen 30430-30585)
            else if ((g_bossPrev || inEp22) && (h == 0x3B47C236u || h == 0x174F53FEu || h == 0x6FFC018Bu || h == 0x9DA45AA1u)) g_hideDraw = true;
        }
    }
}
static void qte_publish() {                                                 // at the swap
    static uint32_t last = 0;
    if (g_qteFrame != last) {
        fprintf(stderr, "[qte] frame %llu: prompt mask %X -> %X\n", (unsigned long long)g_frame, last, g_qteFrame);
        void qte_judge_dump(const char* why);
        char why[32]; snprintf(why, sizeof why, "mask=%X", g_qteFrame);
        qte_judge_dump(why);
    }
    last = g_qteFrame;
    if (g_bossFrame != g_bossPrev) fprintf(stderr, "[qte] frame %llu: boss prompt %s\n", (unsigned long long)g_frame, g_bossFrame ? "on screen (hidden)" : "gone");
    if (g_bossFrame) g_bossMs = GetTickCount64();
    if (g_qteFrame & (QTE_YC | QTE_B)) g_combatMs = GetTickCount64();   // a combat Y/B medallion (xma.cpp: its chime muted in Story Mode)
    g_bossPrev = g_bossFrame; g_bossFrame = false;
    g_qteActive = g_qteFrame;
    g_qteFrame = 0;
}
uint32_t qte_active() { return g_qteActive.load(); }

// ---- texture mods: replacement pictures by content hash ----
// Each texture the game uploads gets a 64-bit hash of its whole picture (the base level as the host holds it, untiled and
// byte-swapped, all formats; width and height folded into the top bits), the same in every run wherever the texture sits in memory.
//   ASURA_MODDUMP=1 writes every distinct texture once to mods\dump\<hash>_f<fmt>_<w>x<h>.png (8 px and up; formats the dumper
//     decodes: RGBA, DXT1-5, R8 and BC4 as grey, R8G8 and BC5 as red and green). ASURA_MODDUMP=<w>x<h>,... keeps only those sizes.
//   mods\textures\**\<hash>*.png replaces the texture whose hash the file name starts with (16 hex digits; the rest of the name
//     is free, so a dumped file can be edited and copied over as it is). Any size with the same aspect ratio: a 4x picture gives
//     4x the detail, as for a higher-resolution font. Channels go in as the dump shows them (grey textures: the red channel counts).
//     Mip levels are generated. Cube maps are not replaced.
//   The Button Labels set (gpu_button_set: 2 PlayStation, 3 Switch) looks in assets\prompts\<set>\ first, the same way.
// Folders are data root (C:\dev\asura in the dev layout) + "\mods"; ASURA_MODS=<dir> names another. Read once at the first texture.
static std::string mods_root() {
    if (const char* e = getenv("ASURA_MODS")) return e;
    return bcr2_data_root() + "\\mods";
}
static uint64_t mods_hash(const uint8_t* p, size_t n) {                     // 64-bit FNV-1a over 8-byte words, then the tail bytes
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001B3ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
struct ModEntry { std::wstring path; HostTex tex; bool tried = false; };
static std::unordered_map<uint64_t, ModEntry> g_mods[4];                    // [0] mods\textures, [2]/[3] the button sets' folders
static bool g_modsScanned = false;
static void mods_scan_dir(const std::wstring& dir, std::unordered_map<uint64_t, ModEntry>& into) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { mods_scan_dir(dir + L"\\" + name, into); continue; }
        if (name.size() < 20 || _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0) continue;
        uint64_t h = 0; bool ok = true;
        for (int i = 0; i < 16 && ok; i++) {
            const wchar_t c = name[i];
            const int v = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
            if (v < 0) ok = false; else h = h << 4 | (uint64_t)v;
        }
        if (ok) into[h].path = dir + L"\\" + name;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
static void mods_scan() {
    if (g_modsScanned) return;
    g_modsScanned = true;
    const std::string r = mods_root() + "\\textures";
    mods_scan_dir(std::wstring(r.begin(), r.end()), g_mods[0]);
    static const wchar_t* sets[4] = { nullptr, nullptr, L"playstation", L"switch" };
    for (int s = 2; s < 4; s++) mods_scan_dir(assets_dir() + L"\\prompts\\" + sets[s], g_mods[s]);
    fprintf(stderr, "[mods] %zu replacement textures in %s; button sets: %zu PlayStation, %zu Switch\n", g_mods[0].size(), r.c_str(), g_mods[2].size(), g_mods[3].size());
}
static bool mods_active() { mods_scan(); return !g_mods[0].empty() || !g_mods[2].empty() || !g_mods[3].empty(); }
static bool mods_dumping() { static const bool on = getenv("ASURA_MODDUMP") != nullptr; return on; }
static HostTex* mods_load(ModEntry& e, uint32_t hw, uint32_t hh) {
    if (e.tried) return e.tex.srv ? &e.tex : nullptr;
    e.tried = true;
    std::vector<uint8_t> px; uint32_t w = 0, h = 0;
    if (!load_png_rgba(e.path, px, w, h) || !w || !h) { fprintf(stderr, "[mods] could not load %ls\n", e.path.c_str()); return nullptr; }
    if ((uint64_t)w * hh != (uint64_t)h * hw) fprintf(stderr, "[mods] %ls is %ux%u, the texture %ux%u: other aspect ratio, stretched\n", e.path.c_str(), w, h, hw, hh);
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &e.tex.tex))) return nullptr;
    g_dev->CreateShaderResourceView(e.tex.tex.Get(), nullptr, &e.tex.srv);
    g_ctx->UpdateSubresource(e.tex.tex.Get(), 0, nullptr, px.data(), w * 4, 0);
    g_ctx->GenerateMips(e.tex.srv.Get());
    // The draw scales texture coordinates by guest size / host size for block formats (bind_tex, tfp): keep the game's padded size
    // here so that factor stays as it was; the sampler works in 0..1 whatever the picture's real size.
    e.tex.hostW = hw; e.tex.hostH = hh; e.tex.levels = 0;
    fprintf(stderr, "[mods] loaded %ls (%ux%u for %ux%u)\n", e.path.c_str(), w, h, hw, hh);
    return e.tex.srv ? &e.tex : nullptr;
}
static HostTex* mods_texture(uint64_t hash, uint32_t hw, uint32_t hh) {
    const int set = gpu_button_set();
    for (int s : { set >= 2 && set <= 3 ? set : -1, 0 }) {
        if (s < 0) continue;
        auto it = g_mods[s].find(hash);
        if (it != g_mods[s].end()) if (HostTex* t = mods_load(it->second, hw, hh)) return t;
    }
    return nullptr;
}
static bool write_png(const std::wstring& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> f; ComPtr<IWICStream> st; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(w, h))) return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;                  // what the PNG encoder takes for 8-bit RGBA
    if (FAILED(fr->SetPixelFormat(&pf)) || pf != GUID_WICPixelFormat32bppBGRA) { static int n = 0; if (n++ < 3) fprintf(stderr, "[moddump] PNG encoder refused 32-bit BGRA\n"); return false; }
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    if (FAILED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data()))) return false;
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}
static void mods_dump(uint64_t hash, const FetchInfo& fi, const TexFmt& tf, uint32_t hw, uint32_t hh, const uint8_t* data) {
    static std::unordered_set<uint64_t> done;
    static int count = 0;
    if ((hw < 8 && hh < 8) || count >= 20000 || !done.insert(hash).second) return;
    if (fi.fmt == 2 && ((fi.w == 1280 && fi.h == 720) || (fi.w == 640 && fi.h == 360))) return;   // Bink movie planes (luma, chroma): a new one every frame
    static const std::string sizes = [] { const char* e = getenv("ASURA_MODDUMP"); return std::string(e && strchr(e, 'x') ? e : ""); }();
    if (!sizes.empty() && ("," + sizes + ",").find(S(",%ux%u,", fi.w, fi.h)) == std::string::npos) return;
    std::vector<uint8_t> rgba;
    if (!decode_rgba(tf, hw, hh, data, rgba)) return;
    static const std::wstring dir = [] {
        const std::string d = mods_root() + "\\dump";
        CreateDirectoryA(mods_root().c_str(), nullptr); CreateDirectoryA(d.c_str(), nullptr);
        return std::wstring(d.begin(), d.end());
    }();
    const std::string n = S("\\%016llX_f%u_%ux%u.png", (unsigned long long)hash, fi.fmt, hw, hh);
    const std::wstring path = dir + std::wstring(n.begin(), n.end());
    if (!write_png(path, rgba, hw, hh)) { DeleteFileW(path.c_str()); return; }
    { count++; fprintf(stderr, "[moddump] f=%llu %08X fmt=%u %ux%u -> %s\n", (unsigned long long)g_frame, fi.base, fi.fmt, hw, hh, n.c_str() + 1); }
}
static bool texdump_wanted(uint32_t base) {
    static const char* only = getenv("ASURA_TEXDUMPONLY");
    if (!only) return true;
    for (const char* p = only; *p;) {                                      // addresses, or ranges lo-hi
        char* e; const uint32_t a = (uint32_t)strtoul(p, &e, 16); if (e == p) break;
        uint32_t b = a;
        if (*e == '-') { const char* q = e + 1; b = (uint32_t)strtoul(q, &e, 16); }
        if (base >= a && base <= b) return true;
        p = (*e == ',') ? e + 1 : e;
    }
    return false;
}
static HostTex* get_texture(const FetchInfo& fi) {
    TexFmt tf;
    if (!tex_fmt(fi.fmt, tf)) { logf_once(S("texf%u", fi.fmt).c_str(), "unsupported texture format %u (%ux%u at %X)", fi.fmt, fi.w, fi.h, fi.base); return nullptr; }
    uint32_t blocksW = align_up(fi.w, tf.block) / tf.block, blocksH = align_up(fi.h, tf.block) / tf.block;
    uint32_t pitchBlocks = std::max(fi.pitch ? fi.pitch / tf.block : blocksW, 1u);
    uint32_t bl2 = 0; while ((1u << bl2) < tf.bpb) bl2++;
    uint32_t pitchAligned = align_up(pitchBlocks, 32), rows = align_up(blocksH, 32);
    uint32_t rowBytes = align_up(pitchBlocks * tf.bpb, 256);
    uint32_t sliceBytes = fi.tiled ? align_up(pitchAligned * rows * tf.bpb, 4096) : align_up(rowBytes * blocksH, 4096);
    uint32_t total = sliceBytes * fi.faces;
    // small packed textures keep their base level inside a tail offset
    uint32_t offX = 0, offY = 0;
    if (fi.packed) {
        uint32_t lw = log2_ceil(fi.w), lh = log2_ceil(fi.h), lmin = std::min(lw, lh);
        if (lmin <= 4) { if (lw > lh) offY = 16; else offX = 16; offX /= tf.block; offY /= tf.block; }
    }
    // Tiled textures whose fetch pitch is at least twice their width were once found with the image at (width, height) inside the tile
    // (explosion sprites, Gohma swarms, before memexport was implemented). Now only a fallback, used when that is where the data is (see below).
    // ASURA_NOSUBRECT turns it off; ASURA_SUBRECTLOG logs each such texture.
    static const bool kSubRect = getenv("ASURA_NOSUBRECT") == nullptr;
    if (fi.tiled && fi.pitch >= 2 * fi.w && !offX && !offY) {
        {   // which textures the rule touches: their shape, and how much data sits at the origin versus at (w, h)
            auto filled = [&](uint32_t bx, uint32_t by) {
                const uint32_t bw = std::max(1u, align_up(fi.w, tf.block) / tf.block), bh = std::max(1u, align_up(fi.h, tf.block) / tf.block);
                uint32_t n = 0, all = 0;
                for (uint32_t y = 0; y < bh; y += std::max(1u, bh / 8)) for (uint32_t x = 0; x < bw; x += std::max(1u, bw / 8)) {
                    all++; if (*(const uint32_t*)phys(fi.base + tiled2d(bx + x, by + y, pitchAligned, bl2))) n++;
                }
                return std::pair<uint32_t, uint32_t>(n, all);
            };
            auto a = filled(0, 0), b = filled(fi.w / tf.block, fi.h / tf.block);
            static const bool kLog = getenv("ASURA_SUBRECTLOG") != nullptr;
            if (kLog) logf_once(S("subr%X_%u_%u", fi.base, fi.w, fi.h).c_str(), "sub-rect candidate %08X fmt %u %ux%u pitch %u packed %d mips %u: data at origin %u/%u, at (w,h) %u/%u",
                                fi.base, fi.fmt, fi.w, fi.h, fi.pitch, (int)fi.packed, fi.maxLevel, a.first, a.second, b.first, b.second);
            // Since memexport is done, every such texture has its image at the origin and nothing at (w, h): the earlier finding was textures caught
            // mid-move. Read from (w, h) only if that is where the data is; always reading there lost Asura's flames.
            if (kSubRect && b.first > a.first) { offX = fi.w / tf.block; offY = fi.h / tf.block; }
        }
    }
    uint64_t key = fnv64(&fi, sizeof(fi));
    auto& slot = g_texCache[key];
    if (!slot) slot = new HostTex();
    HostTex* ht = slot;
    if (!g_buttonKeys.empty() && g_buttonKeys.count(key)) if (HostTex* o = button_override()) return o;
    static const bool kTexDbgWide = getenv("ASURA_TEXDBGWIDE") != nullptr;   // also, at any frame: 2:1 DXT textures up to 256 wide
    const bool wide21 = kTexDbgWide && fi.w <= 256 && fi.w == 2 * fi.h && (fi.fmt == 18 || fi.fmt == 20);
    if (g_trace > 1000 || wide21) {
        static std::set<uint64_t> seen;
        if (seen.insert(key).second && ((fi.w <= 32 && fi.h <= 32) || wide21)) {                // small textures: show their layout and raw blocks
            fprintf(stderr, "[texdbg] base=%08X fmt=%u %ux%u pitch=%u tiled=%d packed=%d endian=%u off=%u,%u raw@off:", fi.base, fi.fmt, fi.w, fi.h, fi.pitch, (int)fi.tiled, (int)fi.packed, fi.endian, offX, offY);
            uint32_t a0 = fi.tiled ? tiled2d(offX, offY, pitchAligned, bl2) : offY * rowBytes + offX * tf.bpb;
            for (uint32_t k = 0; k < 2; k++) fprintf(stderr, " %08X", *(const uint32_t*)phys(fi.base + a0 + k * 4));
            fprintf(stderr, " raw@0:");
            for (uint32_t k = 0; k < 2; k++) fprintf(stderr, " %08X", *(const uint32_t*)phys(fi.base + k * 4));
            uint32_t nz = 0; for (uint32_t k = 0; k < total; k += 4) if (*(const uint32_t*)phys(fi.base + k)) nz++;
            fprintf(stderr, " nonzero dwords=%u of %u map:", nz, total / 4);
            for (uint32_t k = 0; k < total && k < 32768; k += 256) {         // one character per 256 bytes: '#' all set, '+' some, '.' none
                uint32_t c = 0; for (uint32_t j = 0; j < 256; j += 4) if (*(const uint32_t*)phys(fi.base + k + j)) c++;
                fputc(c == 64 ? '#' : c ? '+' : '.', stderr);
            }
            fputc('\n', stderr);
        }
    }
    if (ht->lastFrame == g_frame && ht->mxEpoch == g_mxEpoch && (ht->srv || ht->mod)) return ht->mod ? ht->mod : ht;
    ht->lastFrame = g_frame; ht->mxEpoch = g_mxEpoch;
    PerfScope perfTex(g_tTex);
    static const bool kNoMips = getenv("ASURA_NOMIPS") != nullptr;           // for comparison: draw every texture from its full-size image only
    const uint32_t maxLevel = kNoMips ? 0 : fi.maxLevel, packedLevel = fi.packed ? packed_mip_level(fi.w, fi.h) : ~0u;
    MipStore store[16];
    uint32_t mipBytes = 0;
    for (uint32_t s = packedLevel == 0 ? 0 : 1; maxLevel && s <= std::min(maxLevel, packedLevel) && s < 16; s++) {
        uint32_t pb = align_up(align_up(std::max(next_pow2(fi.w) >> s, 1u), tf.block) / tf.block, 32);
        uint32_t rb = pb * tf.bpb;
        if (!fi.tiled) rb = align_up(rb, 256);
        uint32_t mrows = align_up(align_up(std::max(next_pow2(fi.h) >> s, 1u), tf.block) / tf.block, 32);
        store[s].offset = mipBytes; store[s].pitchBlocks = pb; store[s].rowBytes = rb; store[s].stride = align_up(rb * mrows, 4096);
        mipBytes += store[s].stride * fi.faces;
    }
    uint64_t h = range_hash(fi.base, total);
    if (mipBytes) h ^= range_hash(fi.mipBase, mipBytes) * 0x9E3779B97F4A7C15ull;
    if (fi.fmt == 6 && fi.w == 256 && fi.h == 256 && fi.faces == 1) {
        static std::set<uint64_t> seenAtlas;
        if (seenAtlas.insert(key ^ h).second) fprintf(stderr, "[buttons] 256x256 RGBA texture at %08X hash %016llX%s\n", fi.base, (unsigned long long)h, (kButtonAtlasHash && h == kButtonAtlasHash) ? " (the button atlas)" : "");
        if (kButtonAtlasHash && h == kButtonAtlasHash) g_buttonKeys.insert(key);
    }
    if ((ht->srv || ht->mod) && h == ht->hash) return ht->mod ? ht->mod : ht;
    ht->hash = h;
    ht->mod = nullptr;
    g_nUploads++; g_upBytes += total + mipBytes;
    uint32_t hw = blocksW * tf.block, hh = blocksH * tf.block;
    uint32_t outBpb = tf.conv ? 4 : tf.bpb;
    std::vector<uint8_t> out((size_t)blocksW * blocksH * outBpb * fi.faces);
    uint8_t tmp[16];
    for (uint32_t f = 0; f < fi.faces; f++) {
        uint32_t faceBase = fi.base + sliceBytes * f;
        uint8_t* dstFace = out.data() + (size_t)f * blocksW * blocksH * outBpb;
        for (uint32_t y = 0; y < blocksH; y++) {
            uint8_t* dst = dstFace + (size_t)y * blocksW * outBpb;
            for (uint32_t x = 0; x < blocksW; x++, dst += outBpb) {
                uint32_t a = fi.tiled ? tiled2d(x + offX, y + offY, pitchAligned, bl2) : (y + offY) * rowBytes + (x + offX) * tf.bpb;
                memcpy(tmp, phys(faceBase + a), tf.bpb);
                swap_block(tmp, tf.bpb, fi.endian);
                if (tf.conv) unpack_rgba8(tf.conv, tmp, dst); else memcpy(dst, tmp, tf.bpb);
            }
        }
    }
    if (fi.fmt == 20 && fi.faces == 1) {                                    // DXT5: white wherever it is not transparent (a font page; Story Mode's tug of war keeps these)
        bool white = true;
        for (size_t o = 0; white && o + 16 <= out.size(); o += 16) {
            const uint8_t* b = out.data() + o;
            const bool clear = b[0] == 0 && b[1] == 0;                         // both alpha endpoints 0: fully transparent
            if (!clear && !(b[8] == 0xFF && b[9] == 0xFF && b[10] == 0xFF && b[11] == 0xFF)) white = false;
        }
        ht->white = white;
    }
    else ht->white = false;
    if (fi.faces == 1 && (mods_active() || mods_dumping())) {                // texture mods: by a hash of the whole picture (mods_texture)
        const uint64_t full = mods_hash(out.data(), out.size()) ^ ((uint64_t)hw << 48 | (uint64_t)hh << 32);
        if (mods_dumping()) mods_dump(full, fi, tf, hw, hh, out.data());
        if (HostTex* m = mods_texture(full, hw, hh)) { ht->mod = m; m->white = ht->white; return m; }
    }
    if (fi.fmt == 6 && hw == 1024 && hh == 256 && fi.faces == 1 && out.size() == (size_t)1024 * 256 * 4) {
        static std::set<uint64_t> seenSplash;
        if (seenSplash.insert(h).second) fprintf(stderr, "[splash] 1024x256 RGBA texture at %08X hash %016llX%s\n", fi.base, (unsigned long long)h, (kSplashHash && h == kSplashHash) ? " (the save warning)" : "");
        if (kSplashHash && h == kSplashHash) splash_override(out);
    }
    { static const char* kDump = getenv("ASURA_TEXDUMP"); if (kDump && texdump_wanted(fi.base)) dump_texture_bmp(kDump, fi.base, fi.fmt, hw, hh, tf, out.data()); }
    {   // ASURA_TEXZERO=1: log textures (64x64 and up) whose base level is all zero bytes in guest memory when uploaded: data that never arrived
        static const bool kZero = getenv("ASURA_TEXZERO") != nullptr || bcr2_log_requested();   // on with --log (stops at the first nonzero word, so cheap)
        static int zeroLines = 0;
        if (kZero && fi.w >= 64 && fi.h >= 64 && zeroLines < 2000) {
            bool zero = true;
            for (uint32_t o = 0; o < total && zero; o += 4) zero = *(const uint32_t*)phys(fi.base + o) == 0;
            if (zero && ++zeroLines) fprintf(stderr, "[texzero] frame %llu base=%08X %ux%u fmt=%u mipBase=%08X levels=%u\n", (unsigned long long)g_frame, fi.base, fi.w, fi.h, fi.fmt, fi.mipBase, fi.maxLevel);
        }
    }
    const uint32_t levels = maxLevel + 1;
    bool same = ht->tex && ht->hostW == hw && ht->hostH == hh && ht->levels == levels;
    if (!same) {
        ht->tex.Reset(); ht->srv.Reset(); ht->srvSigned.Reset();
        static const bool kNoSnorm = getenv("ASURA_NOSNORM") != nullptr;
        DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN; int ch = 0;
        const bool twin = !kNoSnorm && !tf.conv && snorm_twin(tf.dxgi, &typeless, &ch) != DXGI_FORMAT_UNKNOWN;
        D3D11_TEXTURE2D_DESC d{}; d.Width = hw; d.Height = hh; d.MipLevels = levels; d.ArraySize = fi.faces; d.Format = twin ? typeless : tf.dxgi; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (fi.faces == 6) d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &ht->tex);
        if (FAILED(hr)) { logf_once(S("texc%u", fi.fmt).c_str(), "texture creation failed fmt=%u %ux%u hr=%08lX", fi.fmt, hw, hh, hr); return nullptr; }
        ht->srv = make_srv(ht->tex.Get(), tf.dxgi, fi.faces);
        ht->view = twin ? tf.dxgi : DXGI_FORMAT_UNKNOWN; ht->faces = fi.faces;
        ht->hostW = hw; ht->hostH = hh; ht->levels = levels;
    }
    for (uint32_t f = 0; f < fi.faces; f++)
        g_ctx->UpdateSubresource(ht->tex.Get(), D3D11CalcSubresource(0, f, levels), nullptr, out.data() + (size_t)f * blocksW * blocksH * outBpb,
                                 blocksW * outBpb, blocksW * blocksH * outBpb);
    for (uint32_t l = 1; l <= maxLevel; l++) {
        const uint32_t s = std::min(l, packedLevel);
        const MipStore& ms = store[s];
        uint32_t ox = 0, oy = 0;
        if (l >= packedLevel) packed_mip_offset(fi.w, fi.h, l, tf.block, ox, oy);
        // what the host level holds, and what the guest stores; they differ only when the base size isn't a whole number of blocks
        uint32_t hbw = align_up(std::max(hw >> l, 1u), tf.block) / tf.block, hbh = align_up(std::max(hh >> l, 1u), tf.block) / tf.block;
        uint32_t gbw = std::min(hbw, align_up(std::max(fi.w >> l, 1u), tf.block) / tf.block), gbh = std::min(hbh, align_up(std::max(fi.h >> l, 1u), tf.block) / tf.block);
        std::vector<uint8_t> lvl((size_t)hbw * hbh * outBpb);
        for (uint32_t f = 0; f < fi.faces; f++) {
            uint32_t faceBase = fi.mipBase + ms.offset + ms.stride * f;
            for (uint32_t y = 0; y < gbh; y++) {
                uint8_t* dst = lvl.data() + (size_t)y * hbw * outBpb;
                for (uint32_t x = 0; x < gbw; x++, dst += outBpb) {
                    uint32_t a = fi.tiled ? tiled2d(x + ox, y + oy, ms.pitchBlocks, bl2) : (y + oy) * ms.rowBytes + (x + ox) * tf.bpb;
                    memcpy(tmp, phys(faceBase + a), tf.bpb);
                    swap_block(tmp, tf.bpb, fi.endian);
                    if (tf.conv) unpack_rgba8(tf.conv, tmp, dst); else memcpy(dst, tmp, tf.bpb);
                }
            }
            g_ctx->UpdateSubresource(ht->tex.Get(), D3D11CalcSubresource(l, f, levels), nullptr, lvl.data(), hbw * outBpb, hbw * hbh * outBpb);
            static const char* kDumpMips = getenv("ASURA_TEXDUMPMIPS");      // dir: every mip level too (files <base + level>_f<fmt>_<w>x<h>)
            if (kDumpMips && f == 0 && hbw * tf.block >= 8 && texdump_wanted(fi.base)) {
                if (l == 1) {
                    fprintf(stderr, "[mipdump] base=%08X %ux%u fmt=%u mipBase=%08X (base + %X; base level %X bytes) levels=%u packedLevel=%u tiled=%d\n",
                            fi.base, fi.w, fi.h, fi.fmt, fi.mipBase, fi.mipBase - fi.base, total, maxLevel, packedLevel, (int)fi.tiled);
                    if (fi.w <= 256 && fi.h <= 256 && tf.block == 4) {                    // the raw tile(s), to work out the layout offline: base level then mips
                        std::string p = std::string(kDumpMips) + S("\\raw_%08X_f%u_%ux%u.bin", fi.base, fi.fmt, fi.w, fi.h);
                        if (FILE* rf = fopen(p.c_str(), "wb")) { for (uint32_t o = 0; o < total + mipBytes; o += 4) { uint32_t v = *(const uint32_t*)phys(fi.base + o); fwrite(&v, 4, 1, rf); } fclose(rf); }
                    }
                }
                dump_texture_bmp(kDumpMips, fi.base + l, fi.fmt, hbw * tf.block, hbh * tf.block, tf, lvl.data());
            }
        }
    }
    return ht;
}

// ---- pipeline state caches ----
static std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> g_blendStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> g_dsStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> g_rsStates;
static std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> g_samplers;

static D3D11_BLEND blend_factor(uint32_t f, bool alpha) {
    switch (f) {
      case 0: return D3D11_BLEND_ZERO;
      case 1: return D3D11_BLEND_ONE;
      case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
      case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
      case 6: return D3D11_BLEND_SRC_ALPHA;
      case 7: return D3D11_BLEND_INV_SRC_ALPHA;
      case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
      case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
      case 10: return D3D11_BLEND_DEST_ALPHA;
      case 11: return D3D11_BLEND_INV_DEST_ALPHA;
      case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
      case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
      case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    }
    return D3D11_BLEND_ONE;
}
static D3D11_BLEND_OP blend_op(uint32_t o) {
    switch (o) { case 1: return D3D11_BLEND_OP_SUBTRACT; case 2: return D3D11_BLEND_OP_MIN; case 3: return D3D11_BLEND_OP_MAX; case 4: return D3D11_BLEND_OP_REV_SUBTRACT; }
    return D3D11_BLEND_OP_ADD;
}
static ID3D11BlendState* get_blend(uint32_t mask) {
    uint32_t bc[4] = { g_regs[R_BLEND0], g_regs[R_BLEND1], g_regs[R_BLEND1 + 1], g_regs[R_BLEND1 + 2] };
    uint64_t k = fnv64(bc, sizeof(bc)); k = fnv64(&mask, 4, k);
    auto& e = g_blendStates[k];
    if (e) return e.Get();
    D3D11_BLEND_DESC d{}; d.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 4; i++) {
        auto& t = d.RenderTarget[i];
        uint32_t b = bc[i];
        uint32_t cs = b & 31, co = (b >> 5) & 7, cd = (b >> 8) & 31, as = (b >> 16) & 31, ao = (b >> 21) & 7, ad = (b >> 24) & 31;
        t.BlendEnable = !(cs == 1 && cd == 0 && co == 0 && as == 1 && ad == 0 && ao == 0);
        t.SrcBlend = blend_factor(cs, false); t.DestBlend = blend_factor(cd, false); t.BlendOp = blend_op(co);
        t.SrcBlendAlpha = blend_factor(as, true); t.DestBlendAlpha = blend_factor(ad, true); t.BlendOpAlpha = blend_op(ao);
        if (co == 2 || co == 3) { t.SrcBlend = D3D11_BLEND_ONE; t.DestBlend = D3D11_BLEND_ONE; }
        if (ao == 2 || ao == 3) { t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_ONE; }
        t.RenderTargetWriteMask = (mask >> (4 * i)) & 0xF;
    }
    g_dev->CreateBlendState(&d, &e);
    return e.Get();
}
static D3D11_STENCIL_OP sten_op(uint32_t o) {
    static const D3D11_STENCIL_OP m[8] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR_SAT,
                                           D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR };
    return m[o & 7];
}
static D3D11_COMPARISON_FUNC cmp_func(uint32_t f) { return (D3D11_COMPARISON_FUNC)((f & 7) + 1); }   // same order as D3D11, shifted by one
static ID3D11DepthStencilState* get_ds(uint32_t dc, uint32_t sr, uint32_t srbf) {
    uint32_t in[3] = { dc, sr & 0xFFFF00, srbf & 0xFFFF00 };
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_dsStates[k];
    if (e) return e.Get();
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = (dc >> 1) & 1;
    d.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = cmp_func((dc >> 4) & 7);
    d.StencilEnable = dc & 1;
    d.StencilReadMask = (sr >> 8) & 0xFF; d.StencilWriteMask = (sr >> 16) & 0xFF;
    d.FrontFace = { sten_op((dc >> 11) & 7), sten_op((dc >> 17) & 7), sten_op((dc >> 14) & 7), cmp_func((dc >> 8) & 7) };
    if ((dc >> 7) & 1) d.BackFace = { sten_op((dc >> 23) & 7), sten_op((dc >> 29) & 7), sten_op((dc >> 26) & 7), cmp_func((dc >> 20) & 7) };
    else d.BackFace = d.FrontFace;
    g_dev->CreateDepthStencilState(&d, &e);
    return e.Get();
}
static ID3D11RasterizerState* get_rs(uint32_t mode, bool rect, bool clipOff, float scale, float offset) {
    uint32_t cull = rect ? 0 : (mode & 3), face = (mode >> 2) & 1;
    uint32_t in[5] = { cull, face, (uint32_t)clipOff, 0, 0 }; memcpy(&in[3], &scale, 4); memcpy(&in[4], &offset, 4);
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_rsStates[k];
    if (e) return e.Get();
    D3D11_RASTERIZER_DESC d{}; d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    static const bool kNoCull = getenv("ASURA_NOCULL") != nullptr;           // debugging: draw both faces
    if (kNoCull) d.CullMode = D3D11_CULL_NONE;
    d.FrontCounterClockwise = face == 0;
    d.DepthClipEnable = !clipOff; d.ScissorEnable = TRUE;
    d.SlopeScaledDepthBias = scale; d.DepthBias = (INT)(offset * 16777216.0f);
    g_dev->CreateRasterizerState(&d, &e);
    return e.Get();
}
static ID3D11SamplerState* get_sampler(const uint32_t* tf, const TexUse& u) {
    uint32_t cx = (tf[0] >> 10) & 7, cy = (tf[0] >> 13) & 7, cz = (tf[0] >> 16) & 7;
    uint32_t mag = u.magf >= 0 ? u.magf : (tf[3] >> 19) & 3, min = u.minf >= 0 ? u.minf : (tf[3] >> 21) & 3, mip = u.mipf >= 0 ? u.mipf : (tf[3] >> 23) & 3;
    uint32_t border = tf[5] & 3;
    // LOD range and bias from the fetch constant. Level 0 is skipped when there's no base image.
    uint32_t minL = (tf[4] >> 2) & 15, maxL = (tf[4] >> 6) & 15;
    if (!(tf[5] >> 12)) minL = maxL = 0;
    else { if (maxL < minL) maxL = minL; if (maxL && !(tf[1] >> 12)) minL = std::max(minL, 1u); }
    if (mip == 2) maxL = minL;                                               // base map: never leave the top level
    int lodBias32 = sext((tf[4] >> 12) & 0x3FF, 10);                         // 5 fractional bits
    // Anisotropy as the game asks for it: 1 = off, 2 = 2:1 ... 5 = 16:1. BCR2_ANISO=n replaces any request above off (0 ignores them all).
    static const int kAnisoForce = getenv("ASURA_ANISO") ? atoi(getenv("ASURA_ANISO")) : -1;
    uint32_t anisoField = (tf[3] >> 25) & 7, aniso = (anisoField >= 2 && anisoField <= 5) ? (1u << (anisoField - 1)) : 1;
    if (kAnisoForce >= 0 && aniso > 1) aniso = kAnisoForce > 1 ? (uint32_t)std::min(kAnisoForce, 16) : 1;
    {   // experiment: BCR2_DEPTHPOINT samples depth textures without filtering, since blending depth values gives depths that exist nowhere in the scene
        static const bool kDepthPoint = getenv("ASURA_DEPTHPOINT") != nullptr;
        uint32_t texFmt = tf[1] & 63;
        if (kDepthPoint && (texFmt == 22 || texFmt == 23)) { mag = min = mip = 0; aniso = 1; }
        static std::set<uint64_t> seen;
        if (g_trace > 1000 && (texFmt == 22 || texFmt == 23) && seen.insert(((uint64_t)tf[1] << 8) | u.slot).second)
            fprintf(stderr, "[depthfetch] slot=%u base=%08X fmt=%u mag=%u min=%u mip=%u (fetch %u %u %u, shader %d %d %d)\n", u.slot, tf[1] & 0xFFFFF000u, texFmt, mag, min, mip, (tf[3] >> 19) & 3, (tf[3] >> 21) & 3, (tf[3] >> 23) & 3, u.magf, u.minf, u.mipf);
    }
    {   // the player's texture filtering: only for textures the game already filters (a point-sampled one is probably data, not a picture)
        const int tfm = g_texFilter.load();
        const uint32_t tfmt = tf[1] & 63;
        if (tfm > 0 && (mag == 1 || min == 1) && mip != 2 && tfmt != 22 && tfmt != 23) {
            mag = min = 1;
            if (tfm == 1) { mip = 0; aniso = 1; }
            else if (tfm == 2) { mip = 1; aniso = 1; }
            else { mip = 1; aniso = 2u << (tfm - 3); }
        }
    }
    uint32_t in[11] = { cx, cy, cz, mag, min, mip, border, minL, maxL, (uint32_t)lodBias32, aniso };
    uint64_t k = fnv64(in, sizeof(in));
    auto& e = g_samplers[k];
    if (e) return e.Get();
    auto am = [](uint32_t c) {
        switch (c) {
          case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
          case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
          case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
          case 3: case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
          default: return D3D11_TEXTURE_ADDRESS_BORDER;
        }
    };
    D3D11_SAMPLER_DESC d{};
    bool lm = mag == 1, ln = min == 1, lp = mip == 1;
    d.Filter = (D3D11_FILTER)((lp ? 0x1 : 0) | (lm ? 0x4 : 0) | (ln ? 0x10 : 0));
    if (aniso > 1 && lm && ln) d.Filter = D3D11_FILTER_ANISOTROPIC;
    d.AddressU = am(cx); d.AddressV = am(cy); d.AddressW = am(cz);
    d.ComparisonFunc = D3D11_COMPARISON_NEVER; d.MinLOD = (float)minL; d.MaxLOD = (float)maxL; d.MipLODBias = lodBias32 / 32.0f; d.MaxAnisotropy = aniso;
    if (border == 1) d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
    g_dev->CreateSamplerState(&d, &e);
    return e.Get();
}

// ---- draw ----
static bool g_probeQLight = false;                                          // BCR2_PROBEQLIGHT: probe the BCR2_DUMPFRAME range without the draw trace, which changes timing
struct ProbeQ { ComPtr<ID3D11Texture2D> st; ID3D11Resource* res = nullptr; std::string text; };
static std::vector<ProbeQ> g_probeQ;
static void probeq_report() {
    std::map<ID3D11Resource*, uint32_t> last;
    size_t n = 0;
    for (auto& q : g_probeQ) {
        n++;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(q.st.Get(), 0, D3D11_MAP_READ, 0, &ms))) continue;
        uint32_t v = *(uint32_t*)ms.pData; g_ctx->Unmap(q.st.Get(), 0);
        auto it = last.find(q.res);
        if (it == last.end() || it->second != v)
            fprintf(stderr, "[probeq] draw#%zu rt=%p %08X -> %08X %s\n", n, q.res, it == last.end() ? 0u : it->second, v, q.text.c_str());
        last[q.res] = v;
    }
    fprintf(stderr, "[probeq] %zu draws read\n", g_probeQ.size());
    g_probeQ.clear();
}
static std::vector<uint32_t> g_vsCode, g_psCode;
static uint64_t g_vsGen = 0, g_psGen = 0;                                    // bumped when the loaded shader code really changes (issue_draw reuses its lookup)
static uint32_t g_vsHash = 0, g_psHash = 0;
static XShader* g_vsCur = nullptr; static XShader* g_psCur = nullptr; static uint32_t g_psParamGen = 0xFFFFFFFFu;

struct alignas(16) StateCb {
    uint32_t bools[8]; uint32_t loops[32];
    float vpNdc[4]; float vpZ[4]; float vpW[4]; float alpha[4]; float vpSnap[4];
    uint32_t vfo[96];
    int32_t tfsw[32 * 4];
    float tfp[32 * 4];
    uint32_t tfsg[32 * 4];
    float tfi[32 * 4];
    uint32_t vsf[32 * 4];
    int32_t vsx[64];
};

static uint32_t vb_alloc(uint32_t size, uint8_t*& ptr) {
    size = align_up(size, 16);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (g_noOverwriteSrv) {
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_vbPos + size > kVbRingSize) {
            g_ctx->End(g_vbDone[g_vbIndex].Get());
            g_vbUsed[g_vbIndex] = true;
            g_vbIndex = (g_vbIndex + 1) % kVbRings; g_vbGen++;
            g_vbRing = g_vbRings[g_vbIndex]; g_vbSrv = g_vbSrvs[g_vbIndex];
            g_vbPos = 0;
            // Normally that ring is long finished. If not, as in a burst of huge frames, a discard is the safe way to get memory.
            BOOL done = FALSE;
            if (!g_vbUsed[g_vbIndex] || g_ctx->GetData(g_vbDone[g_vbIndex].Get(), &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) mode = D3D11_MAP_WRITE_DISCARD;
        } else if (!g_vbUsed[g_vbIndex] && g_vbPos == 0) mode = D3D11_MAP_WRITE_DISCARD;   // the first map of a new buffer
        { PerfScope t(g_sVb); g_ctx->Map(g_vbRing.Get(), 0, mode, 0, &ms); }
        ptr = (uint8_t*)ms.pData + g_vbPos;
    } else {
        static std::vector<uint8_t> scratch; scratch.resize(size); ptr = scratch.data();
        if (g_vbPos + size > kVbRingSize) { g_vbPos = 0; g_vbGen++; }
    }
    uint32_t r = g_vbPos; g_vbPos += size;
    return r;
}
// Makes the next vb_alloc start a fresh ring buffer if this many bytes would not fit in the current one (see issue_draw).
// Returns true when that happens (or the single-buffer fallback will wrap): data copied earlier in the current buffer can't be reused then.
static bool vb_reserve(uint64_t bytes) {
    static uint64_t moves = 0;
    if (g_vbPos > 0 && g_vbPos + bytes > kVbRingSize) {
        if (g_noOverwriteSrv) { g_vbPos = kVbRingSize; if (++moves <= 3) fprintf(stderr, "[gfx] vertex ring: a draw's %llu bytes start the next buffer\n", (unsigned long long)bytes); }
        return true;
    }
    return false;
}
static void vb_commit(uint32_t off, uint32_t size, const uint8_t* ptr) {
    if (g_noOverwriteSrv) { g_ctx->Unmap(g_vbRing.Get(), 0); return; }
    D3D11_BOX box{ off, 0, 0, off + align_up(size, 16), 1, 1 };
    { PerfScope t(g_sVb); g_ctx->UpdateSubresource(g_vbRing.Get(), 0, &box, ptr, 0, 0); }
}
static void upload_cb(ID3D11Buffer* b, const void* p, size_t n) {
    D3D11_MAPPED_SUBRESOURCE ms;
    PerfScope t(g_sCb);
    if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { memcpy(ms.pData, p, n); g_ctx->Unmap(b, 0); }
}

static void compute_scissor(int& x0, int& y0, int& x1, int& y1) {
    uint32_t tl = g_regs[R_WINDOW_TL], br = g_regs[R_WINDOW_BR];
    x0 = tl & 0x3FFF; y0 = (tl >> 16) & 0x3FFF; x1 = br & 0x3FFF; y1 = (br >> 16) & 0x3FFF;
    if (!(tl >> 31)) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        int ox = sext(wo & 0x7FFF, 15), oy = sext((wo >> 16) & 0x7FFF, 15);
        x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    }
    uint32_t stl = g_regs[R_SCREEN_TL], sbr = g_regs[R_SCREEN_BR];
    x0 = std::max(x0, sext(stl & 0x7FFF, 15)); y0 = std::max(y0, sext((stl >> 16) & 0x7FFF, 15));
    x1 = std::min(x1, sext(sbr & 0x7FFF, 15)); y1 = std::min(y1, sext((sbr >> 16) & 0x7FFF, 15));
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::max(x1, x0); y1 = std::max(y1, y0);
}

// From EDRAM rows, as compute_scissor and resolves work in, to where the host keeps them: the negative of an active window offset.
static void host_tile_shift(int& x, int& y) {
    x = y = 0;
    if ((g_regs[R_MODE_CNTL] >> 16) & 1) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        x = -sext(wo & 0x7FFF, 15); y = -sext((wo >> 16) & 0x7FFF, 15);
    }
}

// BCR2_WATCHSURF="hexbase,x,y": during the traced frame, log whenever that pixel of that resolved surface has changed.
static void watch_surface(const char* where) {
    static const char* e = getenv("ASURA_WATCHSURF");
    if (!e || g_trace <= 1000) return;
    uint32_t base = 0, x = 0, y = 0; sscanf(e, "%x,%u,%u", &base, &x, &y);
    auto it = g_surfaces.find(base);
    static uint32_t last = 0xDEADBEEF; static void* lastTex = nullptr;
    if (it == g_surfaces.end()) return;
    Surface* s = it->second;
    D3D11_TEXTURE2D_DESC d; s->tex->GetDesc(&d);
    d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
    g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, s->tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    uint32_t v = *(uint32_t*)ms.pData; g_ctx->Unmap(st.Get(), 0);
    if (v != last || lastTex != s->tex.Get()) { fprintf(stderr, "[watch-surf] %s tex=%p %08X -> %08X\n", where, s->tex.Get(), last, v); last = v; lastTex = s->tex.Get(); }
}

static ID3D11RenderTargetView* g_curRtv[4] = {}; static ID3D11DepthStencilView* g_curDsv = nullptr;
static void invalidate_binds() { memset(g_curRtv, 0, sizeof(g_curRtv)); g_curDsv = nullptr; }

// ASURA_TEXFLIP=1: finds one-frame texture swaps. Each draw (shaders, first vertex stream, its order among such draws in the frame) and texture slot
// remembers what it sampled on the last frames; a value that differs for one frame only and then comes back is logged, saying whether the game bound
// another address (fetch) or the same address held other data (data).
struct FlipVal { uint32_t base = 0, mip = 0, w = 0, h = 0, fmt = 0, src = 0; uint64_t data = 0; bool operator==(const FlipVal& o) const { return base == o.base && mip == o.mip && w == o.w && h == o.h && fmt == o.fmt && src == o.src && data == o.data; } };
struct FlipHist { uint64_t frame = 0; FlipVal v[3]; };
static uint64_t g_flipDraw = 0;                                               // set per draw in issue_draw
static uint64_t g_flipFrame = ~0ull;
static std::unordered_map<uint64_t, uint32_t> g_flipOrd, g_flipOrd1, g_flipOrd2;      // draws per family this frame, last frame, the one before
static uint64_t g_flipFam = 0; static uint32_t g_flipIdx = 0;
static std::unordered_map<uint64_t, FlipHist> g_flipHist;
static bool texflip_on() { static const bool on = getenv("ASURA_TEXFLIP") != nullptr; return on; }
// Also: draws that appear for one frame only, or are missing for one frame only (another LOD or material for a frame). Draws of 300 indices or
// more; frames with more than 40 such changes (camera cuts) are counted, not listed.
struct FlipFam { uint64_t vs, ps; uint32_t vf, idx, num; };
static std::unordered_map<uint64_t, FlipFam> g_flipFams;
static void drawflip_frame() {                                                // at the start of a frame: g_flipOrd = last frame, g_flipOrd1 = the one before, g_flipOrd2 = before that
    std::vector<std::pair<const char*, uint64_t>> ev;
    for (auto& kv : g_flipOrd1) if (!g_flipOrd2.count(kv.first) && !g_flipOrd.count(kv.first)) ev.push_back({ "only", kv.first });
    for (auto& kv : g_flipOrd2) if (!g_flipOrd1.count(kv.first) && g_flipOrd.count(kv.first)) ev.push_back({ "missing", kv.first });
    std::vector<std::pair<const char*, uint64_t>> big;
    for (auto& e : ev) { auto it = g_flipFams.find(e.second); if (it != g_flipFams.end() && it->second.num >= 300) big.push_back(e); }
    if (big.empty()) return;
    if (big.size() > 40) { fprintf(stderr, "[drawflip] frame %llu: %zu draws changed for one frame (camera cut?)\n", (unsigned long long)(g_frame - 2), big.size()); return; }
    for (auto& e : big) {
        const FlipFam& f = g_flipFams[e.second];
        fprintf(stderr, "[drawflip] frame %llu %s for one frame: vs %016llX ps %016llX vf %08X idx %08X num %u\n", (unsigned long long)(g_frame - 2), e.first,
                (unsigned long long)f.vs, (unsigned long long)f.ps, f.vf, f.idx, f.num);
    }
}
static void texflip_draw(uint64_t vsHash, uint64_t psHash, uint32_t vf0, uint32_t idxBase, uint32_t num) {
    if (g_flipFrame != g_frame) {
        if (g_flipFrame + 1 == g_frame) drawflip_frame();
        g_flipOrd2.swap(g_flipOrd1); g_flipOrd1.swap(g_flipOrd); g_flipOrd.clear(); if (g_flipFrame + 1 != g_frame) { g_flipOrd1.clear(); g_flipOrd2.clear(); } g_flipFrame = g_frame;
    }
    uint64_t k = (vsHash * 0x9E3779B97F4A7C15ull ^ psHash ^ ((uint64_t)vf0 << 20)) * 0x100000001B3ull ^ ((uint64_t)idxBase << 24) ^ num;
    if (!g_flipFams.count(k)) g_flipFams[k] = { vsHash, psHash, vf0, idxBase, num };
    g_flipFam = k; g_flipIdx = g_flipOrd[k]++;
    g_flipDraw = k * 31 + g_flipIdx;
}
static void texflip_note(uint32_t slot, const FlipVal& v) {
    FlipHist& h = g_flipHist[g_flipDraw * 64 + slot];
    if (h.frame == g_frame) return;
    if (h.frame + 1 != g_frame) { h = FlipHist{}; h.v[2] = v; h.frame = g_frame; h.v[0].base = h.v[1].base = 0xFFFFFFFFu; return; }
    h.v[0] = h.v[1]; h.v[1] = h.v[2]; h.v[2] = v; h.frame = g_frame;
    // only draws that are alone in their family (same shaders, vertex and index data) on all three frames: others shift when the draw order does
    auto one = [](const std::unordered_map<uint64_t, uint32_t>& m, uint64_t k) { auto it = m.find(k); return it != m.end() && it->second == 1; };
    const bool alone = g_flipIdx == 0 && one(g_flipOrd1, g_flipFam) && one(g_flipOrd2, g_flipFam);
    if (alone && h.v[0].base != 0xFFFFFFFFu && h.v[0] == h.v[2] && !(h.v[1] == h.v[2])) {
        const FlipVal& a = h.v[2]; const FlipVal& b = h.v[1];
        const bool fetch = a.base != b.base || a.mip != b.mip || a.w != b.w || a.h != b.h || a.fmt != b.fmt || a.src != b.src;
        fprintf(stderr, "[texflip] frame %llu (%s) slot %u draw %016llX: %08X/%08X %ux%u fmt %u src %u data %016llX -> for one frame %08X/%08X %ux%u fmt %u src %u data %016llX\n",
                (unsigned long long)(g_frame - 1), fetch ? "fetch" : "data", slot, (unsigned long long)g_flipDraw, a.base, a.mip, a.w, a.h, a.fmt, a.src, (unsigned long long)a.data,
                b.base, b.mip, b.w, b.h, b.fmt, b.src, (unsigned long long)b.data);
    }
}

// ---- memexport draws (see kMemExport) ----
static ComPtr<ID3D11Buffer> g_mxBuf, g_mxStage;
static ComPtr<ID3D11UnorderedAccessView> g_mxUav;
static uint64_t g_mxDraws = 0, g_mxWrites = 0;
static bool mx_ready() {
    static int ok = -1;
    if (ok >= 0) return ok == 1;
    ok = 0;
    if (getenv("ASURA_NOMEMEXPORT")) { fprintf(stderr, "[mx] memexport off (ASURA_NOMEMEXPORT)\n"); return false; }
    if (g_featureLevel < D3D_FEATURE_LEVEL_11_1) { fprintf(stderr, "[mx] feature level 11_1 needed for memexport; those draws are left out\n"); return false; }
    const uint32_t bytes = 16 + kMxCap * 16;
    D3D11_BUFFER_DESC d{}; d.ByteWidth = bytes; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(g_dev->CreateBuffer(&d, nullptr, &g_mxBuf))) { fprintf(stderr, "[mx] buffer creation failed\n"); return false; }
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format = DXGI_FORMAT_R32_TYPELESS; u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; u.Buffer.NumElements = bytes / 4; u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(g_dev->CreateUnorderedAccessView(g_mxBuf.Get(), &u, &g_mxUav))) { fprintf(stderr, "[mx] UAV creation failed\n"); return false; }
    D3D11_BUFFER_DESC s{}; s.ByteWidth = bytes; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateBuffer(&s, nullptr, &g_mxStage))) { fprintf(stderr, "[mx] staging buffer creation failed\n"); return false; }
    fprintf(stderr, "[mx] memexport on: vertex shader writes are read back and applied to guest memory\n");
    ok = 1;
    return true;
}
static void mx_bind() {
    static const uint32_t zero[4] = {};
    D3D11_BOX hb{ 0, 0, 0, 16, 1, 1 };
    g_ctx->UpdateSubresource(g_mxBuf.Get(), 0, &hb, zero, 16, 16);
    ID3D11UnorderedAccessView* u = g_mxUav.Get(); UINT init = 0;
    g_ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 1, &u, &init);
}
// After the draw: unbind, read the records back and write them into guest memory.
static void mx_finish(uint64_t vsHash, uint32_t prim, uint32_t num) {
    ID3D11UnorderedAccessView* nu = nullptr;
    g_ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 1, 1, &nu, nullptr);
    D3D11_BOX hb{ 0, 0, 0, 16, 1, 1 };
    g_ctx->CopySubresourceRegion(g_mxStage.Get(), 0, 0, 0, 0, g_mxBuf.Get(), 0, &hb);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_mxStage.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    const uint32_t count = *(const uint32_t*)ms.pData;
    g_ctx->Unmap(g_mxStage.Get(), 0);
    const uint32_t n = std::min(count, kMxCap);
    if (count > kMxCap) logf_once("mxcap", "memexport draw wrote %u records, more than the %u kept; the rest are lost", count, kMxCap);
    uint32_t lo = 0xFFFFFFFFu, hi = 0, applied = 0;
    if (n) {
        D3D11_BOX rb{ 16, 0, 0, 16 + n * 16, 1, 1 };
        g_ctx->CopySubresourceRegion(g_mxStage.Get(), 0, 16, 0, 0, g_mxBuf.Get(), 0, &rb);
        if (FAILED(g_ctx->Map(g_mxStage.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
        const uint32_t* r = (const uint32_t*)((const uint8_t*)ms.pData + 16);
        for (uint32_t i = 0; i < n; i++, r += 4) {
            const uint32_t a = r[0], v = r[1], m = r[2];
            if (a == 0xFFFFFFFFu) { logf_once(S("mxfmt%u", v).c_str(), "memexport to unsupported format %u (format word %08X)", v, m); continue; }
            if (a >= 0x20000000u) { logf_once("mxaddr", "memexport address %08X outside physical memory", a); continue; }
            uint32_t* p = (uint32_t*)phys(a);
            *p = (*p & ~m) | (v & m);
            lo = std::min(lo, a); hi = std::max(hi, a + 4); applied++;
        }
        g_ctx->Unmap(g_mxStage.Get(), 0);
    }
    g_mxDraws++; g_mxWrites += applied;
    if (applied) g_mxEpoch++;
    static const bool kLog = getenv("ASURA_MXLOG") != nullptr;
    if (g_mxDraws <= 20 || kLog)
        fprintf(stderr, "[mx] f=%llu vs=%016llX prim=%u num=%u dwords=%u range=%08X-%08X\n", (unsigned long long)g_frame, (unsigned long long)vsHash, prim, num, applied, applied ? lo : 0, hi);
}

static void issue_draw(uint32_t init, const uint32_t* inlineIdx, uint32_t dmaBase, uint32_t dmaSize);
// Draw ring (BCR2_SHOTRING): every draw of the last 240 frames, written to draws.txt in the capture folder when F9 is pressed.
struct RingDraw { uint32_t vs, ps, num, vf, pitch, edram, win, vfw, vzero, vhash; uint32_t cvs = 0, cps = 0; uint32_t tex[8] = {}, texh[8] = {}; };
static std::map<uint64_t, std::vector<RingDraw>> g_drawRing;
static void draw_ring_add(uint64_t frame, uint32_t vs, uint32_t ps, uint32_t num, uint32_t vf, uint32_t pitch, uint32_t edram, uint32_t win, uint32_t vfw, uint32_t vzero, uint32_t vhash) {
    auto& v = g_drawRing[frame];
    RingDraw r{ vs, ps, num, vf, pitch, edram, win, vfw, vzero, vhash };
    r.cvs = (uint32_t)fnv64(&g_regs[R_CONST], 64 * 16); r.cps = (uint32_t)fnv64(&g_regs[R_CONST + 256 * 4], 32 * 16);   // vertex c0-63, pixel c256-287
    v.push_back(r);
    while (g_drawRing.size() > 240) g_drawRing.erase(g_drawRing.begin());
}
// the textures the current draw samples (pixel shader slots 0-7): address and a content hash
static void draw_ring_tex(uint32_t slot, uint32_t base, uint64_t hash) {
    if (slot >= 8) return;
    auto it = g_drawRing.find(g_frame);
    if (it == g_drawRing.end() || it->second.empty()) return;
    it->second.back().tex[slot] = base; it->second.back().texh[slot] = (uint32_t)(hash ^ (hash >> 32));
}
static void draw_ring_dump(const char* dir) {
    FILE* f = fopen((std::string(dir) + S("\\draws_%06llu.txt", (unsigned long long)g_frame)).c_str(), "w");   // one file per capture
    if (!f) return;
    for (auto& kv : g_drawRing) {
        fprintf(f, "F %llu %zu\n", (unsigned long long)kv.first, kv.second.size());
        for (auto& d : kv.second) {                                         // ... c=<vertex consts>,<pixel consts> t=<slot 0-7 address:content hash>
            fprintf(f, "%08X %08X %u %08X %u %u %08X %u %u %08X c=%08X,%08X t=", d.vs, d.ps, d.num, d.vf, d.pitch, d.edram, d.win, d.vfw, d.vzero, d.vhash, d.cvs, d.cps);
            for (int i = 0; i < 8; i++) fprintf(f, "%s%08X:%08X", i ? "," : "", d.tex[i], d.texh[i]);
            fputc('\n', f);
        }
    }
    fclose(f);
}
// ASURA_GPUPROF=1 (debug): GPU time per frame and per stretch of work, by D3D11 timestamp queries. A mark starts a new stretch with
// a label: each render-target change (colour base / format / size and depth base), each resolve, the present pass. Results are read
// back 4 frames later without waiting; every 300 frames the average GPU ms per frame and the labels that took the most are logged
// ([gpuprof]). For finding what costs the GPU at a high internal resolution.
static const bool kGpuProf = [] { const char* e = getenv("ASURA_GPUPROF"); return e && *e && strcmp(e, "0") != 0; }();
struct GpuProfFrame { ComPtr<ID3D11Query> disjoint; std::vector<std::pair<ComPtr<ID3D11Query>, std::string>> marks; size_t used = 0; bool open = false; };
static GpuProfFrame g_gp[5];
static int g_gpCur = 0;
static std::map<std::string, double> g_gpAcc;
static double g_gpTotal = 0; static int g_gpFrames = 0, g_gpLate = 0;
static void gpuprof_mark(const std::string& label) {
    if (!kGpuProf || !g_dev) return;
    GpuProfFrame& f = g_gp[g_gpCur];
    if (!f.open) return;
    if (f.used >= 2000) return;
    if (f.used == f.marks.size()) { D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP, 0 }; ComPtr<ID3D11Query> q; g_dev->CreateQuery(&qd, &q); f.marks.push_back({ q, "" }); }
    f.marks[f.used].second = label;
    g_ctx->End(f.marks[f.used].first.Get());
    f.used++;
}
static void gpuprof_frame() {                                               // at each swap: close this frame, read an old one, open the next
    if (!kGpuProf || !g_dev) return;
    GpuProfFrame& cur = g_gp[g_gpCur];
    if (cur.open) { gpuprof_mark("(end)"); g_ctx->End(cur.disjoint.Get()); cur.open = false; }
    g_gpCur = (g_gpCur + 1) % 5;
    GpuProfFrame& f = g_gp[g_gpCur];                                       // the oldest: started 4 frames ago
    if (f.used) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g_ctx->GetData(f.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) g_gpLate++;
        else if (!dj.Disjoint && dj.Frequency) {
            std::vector<UINT64> t(f.used);
            bool ok = true;
            for (size_t i = 0; i < f.used && ok; i++) ok = g_ctx->GetData(f.marks[i].first.Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (ok) {
                const double k = 1000.0 / (double)dj.Frequency;
                for (size_t i = 0; i + 1 < f.used; i++) g_gpAcc[f.marks[i].second] += (double)(t[i + 1] - t[i]) * k;
                g_gpTotal += (double)(t[f.used - 1] - t[0]) * k; g_gpFrames++;
            } else g_gpLate++;
        }
    }
    if (g_gpFrames >= 300) {
        std::vector<std::pair<double, std::string>> v;
        for (auto& [l, ms] : g_gpAcc) v.push_back({ ms / g_gpFrames, l });
        std::sort(v.rbegin(), v.rend());
        std::string out;
        for (size_t i = 0; i < v.size() && i < 12; i++) { char b[160]; snprintf(b, sizeof b, "\n[gpuprof]   %6.2f ms  %s", v[i].first, v[i].second.c_str()); out += b; }
        fprintf(stderr, "[gpuprof] frame=%llu scale=%u: GPU %.2f ms a frame (avg of %d; %d not ready)%s\n", (unsigned long long)g_frame, g_resScale, g_gpTotal / g_gpFrames, g_gpFrames, g_gpLate, out.c_str());
        g_gpAcc.clear(); g_gpTotal = 0; g_gpFrames = 0; g_gpLate = 0;
    }
    if (!f.disjoint) { D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; g_dev->CreateQuery(&qd, &f.disjoint); }
    f.used = 0; f.open = true;
    g_ctx->Begin(f.disjoint.Get());
    gpuprof_mark("(frame start)");
}

static void do_resolve();
static void sync_depth_alias(Rt* depth, uint32_t base, uint32_t pitch);
static void sync_color_alias(Rt* r, uint32_t base, uint32_t fmt, uint32_t pitch);

// The game's fast clear: a depth-only rectangle with z always passing and writing, at half pitch with 4x MSAA. It writes a
// constant dword (D24 << 8 | S8) over that part of EDRAM, which also clears any colour target sharing the base. Host targets are
// separate per pitch and format, so apply it to all of them directly. Returns false to fall back to an ordinary draw.
// withColor is the mode=4 form: the same rectangle also writes a colour taken from its vertices, to the bound colour targets.
static bool is_10bit_family(uint32_t f) { return f == 2 || f == 3 || f == 10 || f == 12; }
static bool is_7e3_float(uint32_t f) { return f == 3 || f == 12; }
// Clear only these host rectangles of a depth target: a scissored draw whose depth comes from the viewport (MinDepth = MaxDepth = z),
// stencil replaced with `stencil`. depthToo false: stencil only. The caller has made the target current (sync_depth_alias).
static void depth_clear_rects(Rt* depth, const std::vector<D3D11_RECT>& rects, float z, UINT8 stencil, bool depthToo) {
    ID3D11ShaderResourceView* nul[32] = {};
    g_ctx->PSSetShaderResources(0, 32, nul);
    g_ctx->OMSetRenderTargets(0, nullptr, depth->dsv.Get());
    invalidate_binds();
    g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
    g_ctx->PSSetShader(nullptr, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    // z on, write, always (or off); stencil on, always, replace on every path; read and write masks FF
    const uint32_t dcClr = 1u | (depthToo ? (2u | 4u | (7u << 4)) : 0u) | (7u << 8) | (2u << 11) | (2u << 14) | (2u << 17);
    g_ctx->OMSetDepthStencilState(get_ds(dcClr, 0xFFFF00u, 0), stencil);
    g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
    D3D11_VIEWPORT vp{ 0, 0, (float)(depth->w * g_resScale), (float)(depth->h * g_resScale), z, z };
    g_ctx->RSSetViewports(1, &vp);
    for (auto& q : rects) { g_ctx->RSSetScissorRects(1, &q); g_ctx->Draw(3, 0); }
    invalidate_binds();
}
// The host rectangles a clear-by-rectangle-list draw covers (the current vertex fetch 0, screen-space positions), clipped to the scissor
// and scaled by samples per pixel and the render scale, in a host target of hostW x hostH. Returns 0: can't tell (clear the whole
// target), 1: covers the whole target, 2: the rectangles in `rects`, 3: covers nothing. A list may hold several rectangles (3 vertices each).
static int clear_rects(uint32_t numVerts, uint32_t msaa, LONG hostW, LONG hostH, std::vector<D3D11_RECT>& rects) {
    rects.clear();
    const uint32_t* vf = &g_regs[R_FETCH];
    static const bool kWhole = getenv("ASURA_FASTCLEARWHOLE") != nullptr;
    static const bool kFirstOnly = getenv("ASURA_FASTCLEARFIRST") != nullptr;   // the old reading (first rectangle, stride words / 3), to compare
    const uint32_t vaddr = vf[0] & ~3u, endian = vf[1] & 3;
    const uint32_t words = (vf[1] >> 2) & 0xFFFFFF, nv = std::max(numVerts, 3u);
    const uint32_t strideW = kFirstOnly ? words / 3 : words / nv;
    const uint32_t nRects = kFirstOnly ? 1 : std::max(nv / 3, 1u);
    if (kWhole || msaa == 1 || strideW < 2) return 0;
    if (g_regs[R_VTE_CNTL] & 0x3F) { logf_once("clearvte", "rectangle clear with viewport scaling on: whole target cleared"); return 0; }
    auto rdv = [&](uint32_t a) { uint32_t w = *(const uint32_t*)phys(a);
        if (endian == 2) w = _byteswap_ulong(w); else if (endian == 1) w = ((w & 0xFF00FF00u) >> 8) | ((w & 0x00FF00FFu) << 8); else if (endian == 3) w = (w >> 16) | (w << 16);
        float f; memcpy(&f, &w, 4); return f; };
    int sx0, sy0, sx1, sy1, tx, ty;
    compute_scissor(sx0, sy0, sx1, sy1); host_tile_shift(tx, ty);
    const int ss = msaa >= 2 ? 2 : 1;                                       // 4x: 2x2 samples per pixel
    for (uint32_t r = 0; r < nRects; r++) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t k = 0; k < 3; k++) {
            const uint32_t va = vaddr + (r * 3 + k) * strideW * 4;
            const float x = rdv(va), y = rdv(va + 4);
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        int rx0 = std::max((int)lroundf(x0), sx0 + tx), ry0 = std::max((int)lroundf(y0), sy0 + ty);
        int rx1 = std::min((int)lroundf(x1), sx1 + tx), ry1 = std::min((int)lroundf(y1), sy1 + ty);
        rx0 = std::max(rx0, 0) * ss * g_resScale; ry0 = std::max(ry0, 0) * ss * g_resScale;
        rx1 = std::min(rx1 * ss * g_resScale, (int)hostW); ry1 = std::min(ry1 * ss * g_resScale, (int)hostH);
        if (rx1 <= rx0 || ry1 <= ry0) continue;                             // this one covers nothing
        if (rx0 > 0 || ry0 > 0 || rx1 < hostW || ry1 < hostH) rects.push_back({ rx0, ry0, rx1, ry1 });
        else { rects.clear(); return 1; }
    }
    return rects.empty() ? 3 : 2;
}
static bool fast_clear(bool withColor, uint32_t numVerts) {
    const uint32_t* vf = &g_regs[R_FETCH];
    if ((vf[0] & 3) != 3) return false;
    uint32_t surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF, msaa = (surf >> 16) & 3;
    if (!pitch) return false;
    uint32_t hostPitch = pitch << (msaa >= 2 ? 1 : 0);                      // host targets hold pixels, the clear covers samples
    uint32_t vaddr = vf[0] & ~3u, endian = vf[1] & 3;
    uint32_t raw = *(const uint32_t*)phys(vaddr + 8);                       // first vertex: x, y, z
    if (endian == 2) raw = _byteswap_ulong(raw);
    else if (endian == 1) raw = ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
    else if (endian == 3) raw = (raw >> 16) | (raw << 16);
    float z; memcpy(&z, &raw, 4);
    z = std::clamp(z, 0.0f, 1.0f);
    uint32_t dc = g_regs[R_DEPTHCONTROL], sr = g_regs[R_STENCIL], stencil = 0;
    if ((dc & 1) && ((dc >> 14) & 7) == 2) stencil = sr & 0xFF;             // z-pass op: replace
    uint32_t d24 = std::min((uint32_t)(z * 16777215.0 + 0.5), 0xFFFFFFu), base = g_regs[R_DEPTH_INFO] & 0xFFF;
    uint32_t word = (d24 << 8) | stencil;
    float col[4] = { (word & 255) / 255.0f, ((word >> 8) & 255) / 255.0f, ((word >> 16) & 255) / 255.0f, (word >> 24) / 255.0f };
    ID3D11RenderTargetView* none[1] = { nullptr };
    g_ctx->OMSetRenderTargets(1, none, nullptr);
    invalidate_binds();
    Rt* depth = get_depth_rt(base, hostPitch);
    // The rectangle it covers. UE3 packs every shadow caster's depth map into one atlas at the same EDRAM base and clears each
    // caster's tile this way before drawing it; clearing the whole host target erased every caster drawn before, so only the last
    // one's shadow map survived and the crowd's lighting popped as the draw order changed (episode 1, Shinkoku). Host rows are the
    // vertex positions (a window offset does not move host targets), clipped to the scissor; samples per pixel scale them.
    D3D11_RECT clr{ 0, 0, (LONG)(depth->w * g_resScale), (LONG)(depth->h * g_resScale) };
    bool partial = false;
    // A rectangle list may hold several rectangles (3 vertices each). UE3 clears a shadow-atlas tile that isn't EDRAM-tile aligned with
    // a 4x MSAA rectangle for the aligned part and a two-rectangle 1x list for the strips around it; reading only the first three
    // vertices (with the stride taken as words / 3) built one wrong rectangle from vertices 0, 2 and 4 and left a strip uncleared,
    // so depth from earlier casters stayed in the tile: straight-edged shadow fragments with no caster (episode 8's crowd).
    std::vector<D3D11_RECT> rects;
    {
        const int got = clear_rects(numVerts, msaa, clr.right, clr.bottom, rects);
        if (got == 3) return true;                                              // nothing covered
        if (got == 2) {
            partial = true;
            clr = rects[0];
            for (auto& q : rects) { clr.left = std::min(clr.left, q.left); clr.top = std::min(clr.top, q.top); clr.right = std::max(clr.right, q.right); clr.bottom = std::max(clr.bottom, q.bottom); }
        }
    }
    if (!partial) g_ctx->ClearDepthStencilView(depth->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, (UINT8)stencil);
    else { sync_depth_alias(depth, base, hostPitch); depth_clear_rects(depth, rects, z, (UINT8)stencil, true); }   // the rest of the target keeps its contents
    depth->seq = ++g_rtSeq;
    // Every colour target at this base and pitch shares those EDRAM bits, whatever its format. UE3 clears its 7e3 scene target
    // (format 3) this way each frame; clearing only the 8-bit views left the scene uncleared wherever no geometry drew (the
    // window behind the emperor in episode 1 kept the previous shot and smeared). ASURA_FASTCLEAR8 restores the 8-bit-only clear.
    static const bool kOnly8 = getenv("ASURA_FASTCLEAR8") != nullptr;
    auto from7e3 = [](uint32_t v) { const uint32_t m = v & 0x7F, e = v >> 7; return e == 0 ? m / 512.0f : (1.0f + m / 128.0f) * exp2f((float)e - 3.0f); };
    for (auto& kv : g_rts) {
        if ((uint32_t)(kv.first >> 32) != base || (kv.first & 0xFFFF) != (hostPitch & 0xFFFF)) continue;
        const uint32_t f = (uint32_t)(kv.first >> 16) & 0xFFFF;
        float c[4];
        if (f == 0 || f == 1) memcpy(c, col, sizeof c);
        else if (kOnly8) continue;
        else if (word == 0) c[0] = c[1] = c[2] = c[3] = 0.0f;
        else if (is_10bit_family(f)) {
            const uint32_t ch[3] = { word & 0x3FF, (word >> 10) & 0x3FF, (word >> 20) & 0x3FF };
            for (int i = 0; i < 3; i++) c[i] = is_7e3_float(f) ? from7e3(ch[i]) : ch[i] / 1023.0f;
            c[3] = (word >> 30) / 3.0f;
        } else continue;                                                    // 64-bit and other formats: only a zero word is known to be safe
        if (partial) { if (g_ctx1) g_ctx1->ClearView(kv.second->rtv.Get(), c, rects.data(), (UINT)rects.size()); }
        else g_ctx->ClearRenderTargetView(kv.second->rtv.Get(), c);
        kv.second->seq = g_rtSeq - 1;
    }
    if (withColor) {
        float vc[4];
        for (int i = 0; i < 4; i++) {
            uint32_t w = *(const uint32_t*)phys(vaddr + 12 + i * 4);
            if (endian == 2) w = _byteswap_ulong(w);
            else if (endian == 1) w = ((w & 0xFF00FF00u) >> 8) | ((w & 0x00FF00FFu) << 8);
            else if (endian == 3) w = (w >> 16) | (w << 16);
            memcpy(&vc[i], &w, 4);
        }
        static const uint32_t regs[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
        uint32_t mask = g_regs[R_COLOR_MASK];
        for (int i = 0; i < 4; i++) {
            if (((mask >> (4 * i)) & 0xF) == 0) continue;
            uint32_t ci = g_regs[regs[i]];
            uint64_t key = ((uint64_t)(ci & 0xFFF) << 32) | ((uint64_t)((ci >> 16) & 0xF) << 16) | (hostPitch & 0xFFFF);
            if (i == 0 || g_rts.count(key)) {                               // later slots may hold stale registers: only touch known targets
                Rt* r = get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, hostPitch);
                if (partial) { if (g_ctx1) g_ctx1->ClearView(r->rtv.Get(), vc, rects.data(), (UINT)rects.size()); }
                else g_ctx->ClearRenderTargetView(r->rtv.Get(), vc);
                r->seq = g_rtSeq - 1;
            }
        }
    }
    static int nl = 0;
    if (nl++ < 6 || g_trace > 0) fprintf(stderr, "[gfx] fast clear base=%X pitch=%u(%u) z=%g stencil=%u word=%08X rect=%ld,%ld-%ld,%ld%s\n", base, pitch, hostPitch, z, stencil, word,
                                         clr.left, clr.top, clr.right, clr.bottom, partial ? S(" (part, %zu)", rects.size()).c_str() : "");
    if (g_trace > 0 && rects.size() > 1) for (auto& q : rects) fprintf(stderr, "[gfx]   clear rect %ld,%ld-%ld,%ld\n", q.left, q.top, q.right, q.bottom);
    return true;
}

// The slowest single draw of the frame, for the [spike] line: one stalled call and evenly slow draws need different fixes.
static double g_worstDrawMs = 0, g_wCb = 0, g_wVb = 0, g_wIb = 0, g_wCall = 0, g_wRt = 0; static uint64_t g_worstVs = 0; static uint32_t g_worstNum = 0, g_worstSurf = 0;
struct WorstDraw {
    uint32_t num; LARGE_INTEGER a;
    explicit WorstDraw(uint32_t n) : num(n) { g_sCb = g_sVb = g_sIb = g_sCall = g_sRt = 0; QueryPerformanceCounter(&a); }
    ~WorstDraw() { LARGE_INTEGER b; QueryPerformanceCounter(&b); double ms = (double)(b.QuadPart - a.QuadPart) * g_perfTicksToMs; if (ms > g_worstDrawMs) { g_worstDrawMs = ms; g_worstVs = g_vsHash; g_worstNum = num; g_worstSurf = g_regs[R_SURFACE_INFO]; g_wCb = g_sCb; g_wVb = g_sVb; g_wIb = g_sIb; g_wCall = g_sCall; g_wRt = g_sRt; } }
};

// The current shaders, looked up again only when the loaded code changed (build_shader hashes the whole microcode).
static XShader* cur_vs() {
    static uint64_t gen = ~0ull; static XShader* s = nullptr;
    if (!s || gen != g_vsGen) { s = vs_variant(g_vsCode); gen = g_vsGen; }
    return s;
}
static XShader* cur_ps(uint32_t paramGen) {
    static uint64_t gen = ~0ull; static uint32_t pg = 0; static XShader* s = nullptr;
    if (!s || gen != g_psGen || pg != paramGen) { s = build_shader(g_psCode, false, paramGen); gen = g_psGen; pg = paramGen; }
    return s;
}
// ASURA_DRAWPROF=1: time per phase of issue_draw, summed per frame and logged every 300 frames as [drawprof] (ms per frame).
static const bool kDrawProf = getenv("ASURA_DRAWPROF") != nullptr;
static double g_dp[10] = {}, g_dpFrame[10] = {};                            // g_dpFrame: this frame only, for a [hitch] line
static LARGE_INTEGER g_dpLast;
static const char* const kDpNames[10] = { "setup", "shaders", "targets", "indices", "vertices", "textures", "constants", "state", "-", "call+tail" };
#define DP(k) do { if (kDrawProf) { LARGE_INTEGER n_; QueryPerformanceCounter(&n_); const double d_ = (double)(n_.QuadPart - g_dpLast.QuadPart) * g_perfTicksToMs; g_dp[k] += d_; g_dpFrame[k] += d_; g_dpLast = n_; } } while (0)
struct DpScope { DpScope() { if (kDrawProf) QueryPerformanceCounter(&g_dpLast); } ~DpScope() { DP(9); } };
// [vram] with each [perf]: the driver's video memory use against its budget, and how many host textures the cache holds
static void vram_report() {
    static ComPtr<IDXGIAdapter3> adapter; static bool tried = false;
    if (!tried) {
        tried = true;
        ComPtr<IDXGIDevice> d; ComPtr<IDXGIAdapter> a;
        if (SUCCEEDED(g_dev.As(&d)) && SUCCEEDED(d->GetAdapter(&a))) a.As(&adapter);
    }
    size_t n = 0; uint64_t texels = 0;
    for (auto& kv : g_texCache) if (kv.second->tex) { n++; texels += (uint64_t)kv.second->hostW * kv.second->hostH; }
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) i = {};
    fprintf(stderr, "[vram] frame=%llu usage=%lluMB budget=%lluMB textures=%zu base-level texels=%lluM\n", (unsigned long long)g_frame,
            (unsigned long long)(i.CurrentUsage >> 20), (unsigned long long)(i.Budget >> 20), n, (unsigned long long)(texels >> 20));
}
static void drawprof_report(uint64_t frames) {
    if (!kDrawProf || !frames) return;
    std::string s = S("[drawprof] %llu frames, ms per frame:", (unsigned long long)frames);
    for (int k = 0; k < 10; k++) if (k != 8) s += S(" %s=%.2f", kDpNames[k], g_dp[k] / frames);
    fprintf(stderr, "%s\n", s.c_str());
    memset(g_dp, 0, sizeof(g_dp));
}
static void issue_draw(uint32_t init, const uint32_t* inlineIdx, uint32_t dmaBase, uint32_t dmaSize) {
    uint32_t prim = init & 63, srcSel = (init >> 6) & 3, idx32 = (init >> 11) & 1, num = init >> 16;
    uint32_t edram = g_regs[R_MODECONTROL] & 7;
    if (g_trace > 0) {
        g_trace--;
        fprintf(stderr, "[gfx] draw prim=%u src=%u num=%u mode=%u surf=%08X col=%08X dep=%08X pred win=%08X sc=%08X/%08X mask=%08X dc=%08X vte=%08X vs=%016llX ps=%016llX\n",
                prim, srcSel, num, edram, g_regs[R_SURFACE_INFO], g_regs[R_COLOR_INFO], g_regs[R_DEPTH_INFO], g_regs[R_WINDOW_OFFSET], g_regs[R_WINDOW_TL], g_regs[R_WINDOW_BR],
                g_regs[R_COLOR_MASK], g_regs[R_DEPTHCONTROL], g_regs[R_VTE_CNTL], (unsigned long long)g_vsHash, (unsigned long long)g_psHash);
    }
    watch_surface("before draw/resolve");
    if (edram == 6) { PerfScope perfRes(g_tResolve); do_resolve(); watch_surface("after resolve"); return; }
    PerfScope perfDraw(g_tDraw);
    DpScope dpScope;
    WorstDraw worstDraw(num);
    if (num == 0 || prim == 0) return;
    if (edram == 0) {                                                       // no EDRAM output: only a memexport draw does anything
        if (g_vsCode.empty() || !cur_vs()->memexport) return;
    }
    if (prim >= 0x10) { logf_once(S("prim%u", prim).c_str(), "unsupported primitive type %u", prim); return; }
    if (prim == 1) logf_once("points", "point list drawn as 1-pixel points (no sprite expansion yet)");
    if (g_vsCode.empty()) return;
    g_draws++; g_drawsFrame++;
    // Stencil-only form of the same trick: a half-pitch 4x MSAA rectangle, no depth test, stencil always/replace. Host targets
    // are per pitch, so apply it to the real depth buffer. Without this, marks from an earlier pass survive into the composite.
    {
        uint32_t dcS = g_regs[R_DEPTHCONTROL], surfS = g_regs[R_SURFACE_INFO];
        if (prim == 8 && srcSel == 2 && edram == 5 && ((surfS >> 16) & 3) >= 2 && (surfS & 0x3FFF) != 0 && (dcS & 3) == 1 &&
            ((dcS >> 8) & 7) == 7 && ((dcS >> 14) & 7) == 2) {
            uint32_t base = g_regs[R_DEPTH_INFO] & 0xFFF, hostPitch = (surfS & 0x3FFF) << 1;
            Rt* d = get_depth_rt(base, hostPitch);
            sync_depth_alias(d, base, hostPitch);
            ID3D11RenderTargetView* none[1] = { nullptr };
            g_ctx->OMSetRenderTargets(1, none, nullptr);
            invalidate_binds();
            const UINT8 ref = (UINT8)(g_regs[R_STENCIL] & 0xFF);
            // Only the rectangles drawn: clearing the whole stencil buffer wiped marks the game keeps elsewhere in it (the same mistake
            // as the shadow-atlas depth clears). ASURA_FASTCLEARWHOLE=1 clears the whole buffer as before.
            std::vector<D3D11_RECT> rects;
            const int got = (g_regs[R_FETCH] & 3) == 3 ? clear_rects(num, (surfS >> 16) & 3, (LONG)(d->w * g_resScale), (LONG)(d->h * g_resScale), rects) : 0;
            if (got == 2) { depth_clear_rects(d, rects, 0.0f, ref, false); d->seq = ++g_rtSeq; }
            else if (got != 3) g_ctx->ClearDepthStencilView(d->dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, ref);
            static int nl = 0;
            if (nl++ < 4 || g_trace > 0) fprintf(stderr, "[gfx] stencil clear base=%X pitch=%u ref=%u%s\n", base, hostPitch, ref,
                                                 got == 2 ? S(" (part, %zu)", rects.size()).c_str() : got == 3 ? " (nothing)" : "");
            return;
        }
    }
    if (prim == 8 && srcSel == 2 && (g_regs[R_DEPTHCONTROL] & 0x76) == 0x76 &&
        (edram == 5 || (edram == 4 && ((g_regs[R_SURFACE_INFO] >> 16) & 3) != 0)) && fast_clear(edram == 4, num)) return;

    DP(0);
    XShader* vs = cur_vs();
    g_vsHash = (uint32_t)((unsigned long long)vs->hash >> 32);
    // A memexport draw: run its vertex shader once per vertex, writing guest memory, with no render target and no pixel shader.
    const bool mx = vs->memexport && mx_ready();
    if (vs->memexport && !mx) return;
    if (g_shotRingSize > 0) {   // the draw ring: what was drawn in each of the last frames, saved beside the F9 images
        const uint32_t vf0 = vs->vf.empty() ? 0 : (g_regs[R_FETCH + vs->vf[0].slot * 2] & ~3u);
        uint32_t vfw = 0, vzero = 0, vhash = 0;   // the first vertex stream: its size in words, how many are zero, and a checksum (to tell a draw fed stale or empty vertices)
        if (vf0 && !vs->vf.empty()) {
            vfw = (g_regs[R_FETCH + vs->vf[0].slot * 2 + 1] >> 2) & 0xFFFFFF;
            const uint32_t* w = (const uint32_t*)phys(vf0);
            // 256 words spread over the stream (the zero count is scaled): hashing all of it for every draw cost 10+ ms a frame in busy scenes
            const uint32_t step = std::max(1u, vfw / 256);
            uint32_t seen = 0;
            vhash = 2166136261u;
            for (uint32_t i = 0; i < vfw; i += step, seen++) { vzero += w[i] == 0; vhash = (vhash ^ w[i]) * 16777619u; }
            if (seen && step > 1) vzero = (uint32_t)((uint64_t)vzero * vfw / seen);
        }
        draw_ring_add(g_frame, g_vsHash, (uint32_t)((unsigned long long)g_psHash), num, vf0, g_regs[R_SURFACE_INFO] & 0x3FFF, edram, g_regs[R_WINDOW_OFFSET], vfw, vzero, vhash);
    }
    uint32_t misc = g_regs[R_CONTEXT_MISC], pcntl = g_regs[R_PROGRAM_CNTL];
    uint32_t paramGen = ((pcntl >> 18) & 1) ? ((misc >> 8) & 0xFF) : 0xFFFFFFFFu;
    bool depthOnly = edram == 5;
    // Depth-only mode runs no pixel shader, as on the console. The game sets none for such a draw and just leaves the old one loaded, so running
    // that one here gives the mesh the previous draw's texture and alpha cut-out, and its shadow then depends on the draw order. BCR2_DEPTHKEEPPS restores that.
    static const bool kDepthKeepPs = getenv("ASURA_DEPTHKEEPPS") != nullptr;
    static const char* kDepthSolid = getenv("ASURA_DEPTHSOLID");              // comma-separated vertex shader hashes: only these depth-only draws skip the pixel shader
    const bool skipPs = depthOnly && !kDepthKeepPs && (!kDepthSolid || strstr(kDepthSolid, S("%016llX", (unsigned long long)vs->hash).substr(0, 8).c_str()));
    XShader* ps = (g_psCode.empty() || skipPs || mx) ? nullptr : cur_ps(paramGen);
    {   // ASURA_CONSTLOG="<ps hash>:<from>-<to>": in those frames, each draw with that pixel shader logs its vertex count and pixel constants c256-c285
        static const char* kCl = getenv("ASURA_CONSTLOG");
        static uint64_t clHash = 0, clFrom = 0, clTo = 0;
        static bool parsed = false;
        if (kCl && !parsed) { parsed = true; clHash = _strtoui64(kCl, nullptr, 16); if (const char* c = strchr(kCl, ':')) { clFrom = _strtoui64(c + 1, nullptr, 10); if (const char* m = strchr(c, '-')) clTo = _strtoui64(m + 1, nullptr, 10); } }
        if (kCl && ps && ps->hash == clHash && g_frame >= clFrom && g_frame <= clTo) {
            static uint64_t lastF = ~0ull; static int idx = 0;
            if (g_frame != lastF) { lastF = g_frame; idx = 0; }
            std::string s = S("[constlog] f=%llu i=%d vs=%08X n=%u vf=%08X", (unsigned long long)g_frame, idx++, (uint32_t)(vs->hash >> 32), num, g_regs[R_FETCH + 95 * 2] & ~3u);
            for (int k = 256; k <= 285; k++) s += S(" c%d=%.4g,%.4g,%.4g,%.4g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
            fprintf(stderr, "%s\n", s.c_str());
        }
    }
    if (g_trace > 0) {
        fprintf(stderr, "[gfx]   shaders vs=%016llX ok=%d ps=%016llX ok=%d vte=%08X mode=%08X blend0=%08X cc=%08X psconst=%08X tf:", (unsigned long long)vs->hash, (int)vs->ok,
                ps ? (unsigned long long)ps->hash : 0ull, ps ? (int)ps->ok : -1, g_regs[R_VTE_CNTL], g_regs[R_MODE_CNTL], g_regs[R_BLEND0], g_regs[R_COLORCONTROL], g_regs[0x2308]);
        if (ps) for (auto& u : ps->tex) { const uint32_t* t = &g_regs[R_FETCH + u.slot * 6]; fprintf(stderr, " %u:fmt=%u,base=%08X,%ux%u,sw=%03X,sg=%02X,exp=%d", u.slot, t[1] & 63, t[1] & 0xFFFFF000u, (t[2] & 0x1FFF) + 1, ((t[2] >> 13) & 0x1FFF) + 1, (t[3] >> 1) & 0xFFF, (t[0] >> 2) & 0xFF, sext((t[3] >> 13) & 63, 6)); }
        fprintf(stderr, " c509=%g,%g,%g,%g c256=%g,%g,%g,%g c267=%g,%g,%g,%g colfmt=%u", reg_f(R_CONST + 509 * 4), reg_f(R_CONST + 509 * 4 + 1), reg_f(R_CONST + 509 * 4 + 2), reg_f(R_CONST + 509 * 4 + 3),
                reg_f(R_CONST + 256 * 4), reg_f(R_CONST + 256 * 4 + 1), reg_f(R_CONST + 256 * 4 + 2), reg_f(R_CONST + 256 * 4 + 3),
                reg_f(R_CONST + 267 * 4), reg_f(R_CONST + 267 * 4 + 1), reg_f(R_CONST + 267 * 4 + 2), reg_f(R_CONST + 267 * 4 + 3), (g_regs[R_COLOR_INFO] >> 16) & 0xF);
        for (int k : { 259, 263, 508, 510, 511 })                           // post-process constants (tone map: c263 bloom, c508 gamma, c511 decode)
            fprintf(stderr, " c%d=%g,%g,%g,%g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
        static const char* traceVC = getenv("ASURA_TRACEVC");                // hash of a vertex shader: print its transform constants, once per frame
        if (traceVC && S("%016llX", (unsigned long long)vs->hash) == traceVC) {
            static uint64_t lastF = ~0ull;
            if (lastF != g_frame) {
                lastF = g_frame;
                fprintf(stderr, "\n[vconst] frame=%llu", (unsigned long long)g_frame);
                for (int k : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 201, 202, 203, 204, 205, 209, 210, 211, 212, 213, 253, 254, 255 }) fprintf(stderr, " c%d=%.7g,%.7g,%.7g,%.7g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
                fprintf(stderr, "\n");
            }
        }
        static const char* traceC = getenv("ASURA_TRACEC");                  // hash of a pixel shader: also print its constants
        if (traceC && ps && S("%016llX", (unsigned long long)ps->hash) == traceC) {
            fprintf(stderr, " PC:");
            for (int k = 256; k < 276; k++) fprintf(stderr, " c%d=%g,%g,%g,%g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
            for (int k = 508; k < 512; k++) fprintf(stderr, " c%d=%g,%g,%g,%g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
        }
        fprintf(stderr, " vsconst=%08X c0=%g,%g,%g,%g c4=%g,%g,%g,%g zs=%g zo=%g clip=%08X", g_regs[0x2307], reg_f(R_CONST), reg_f(R_CONST + 1), reg_f(R_CONST + 2), reg_f(R_CONST + 3),
                reg_f(R_CONST + 16), reg_f(R_CONST + 17), reg_f(R_CONST + 18), reg_f(R_CONST + 19), reg_f(R_VPORT_XSCALE + 4), reg_f(R_VPORT_XSCALE + 5), g_regs[R_CLIP_CNTL]);
        fprintf(stderr, " vf:");
        for (auto& u : vs->vf) { uint32_t f0 = g_regs[R_FETCH + u.slot * 2], f1 = g_regs[R_FETCH + u.slot * 2 + 1]; fprintf(stderr, " %u:addr=%08X,words=%u,end=%u,stride=%u", u.slot, f0 & ~3u, (f1 >> 2) & 0xFFFFFF, f1 & 3, u.stride); }
        fprintf(stderr, "\n");
    }
    DP(1);
    if (!shaders_ready(vs, ps, prim == 8 || mx)) return;                    // a memexport draw left out would lose the memory it writes
    if (!run_sh(vs)->ok) return;
    if (ps && !ps->ok) ps = nullptr;

    // ---- render targets ----
    uint32_t surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF;
    if (pitch == 0 && !mx) { logf_once("pitch0", "draw with surface pitch 0 ignored"); return; }
    uint32_t mask = g_regs[R_COLOR_MASK];
    ID3D11RenderTargetView* rtvs[4] = {};
    uint32_t rw = 0, rh = 0;
    if (mx) rw = rh = 1;                                                    // no targets; the points rasterise to nothing that is kept
    else if (!depthOnly) {
        static const uint32_t regs[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
        for (int i = 0; i < 4; i++) {
            if (((mask >> (4 * i)) & 0xF) == 0) continue;
            uint32_t ci = g_regs[regs[i]];
            Rt* r = get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, pitch);
            sync_color_alias(r, ci & 0xFFF, (ci >> 16) & 0xF, pitch);
            // Slots can share an EDRAM base. D3D11 rejects a view bound twice and silently keeps the previous targets,
            // which sent such draws into whatever surface the last resolve had bound. Bind each target once.
            bool dup = false;
            for (int k = 0; k < i; k++) if (rtvs[k] == r->rtv.Get()) dup = true;
            if (!dup) rtvs[i] = r->rtv.Get();
            rw = r->w; rh = r->h;
            r->seq = ++g_rtSeq;
        }
    }
    uint32_t dc = g_regs[R_DEPTHCONTROL];
    ID3D11DepthStencilView* dsv = nullptr;
    if ((dc & 3) && !mx) {
        uint32_t depthBase = g_regs[R_DEPTH_INFO] & 0xFFF;
        Rt* d = get_depth_rt(depthBase, pitch);
        sync_depth_alias(d, depthBase, pitch);
        if (dc & 4) d->seq = ++g_rtSeq;                                     // writes depth from here on
        dsv = d->dsv.Get(); rw = d->w; rh = d->h;
    }
    if (!rw) return;                                                        // nothing would be written
    if (memcmp(rtvs, g_curRtv, sizeof(rtvs)) || dsv != g_curDsv) {
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        { PerfScope t(g_sRt); g_ctx->OMSetRenderTargets(4, rtvs, dsv); }
        memcpy(g_curRtv, rtvs, sizeof(rtvs)); g_curDsv = dsv;
        if (kGpuProf) {
            char b[96]; const uint32_t c0 = g_regs[R_COLOR_INFO];
            snprintf(b, sizeof b, "draw c%03X/f%u %ux%u d%03X", rtvs[0] ? (c0 & 0xFFF) : 0xFFF, rtvs[0] ? (c0 >> 16) & 0xF : 0, rw, rh, dsv ? (g_regs[R_DEPTH_INFO] & 0xFFF) : 0xFFF);
            gpuprof_mark(b);
        }
    }

    DP(2);
    // ---- indices ----
    static std::vector<uint32_t> idx;
    idx.clear();
    uint32_t mode = g_regs[R_MODE_CNTL];
    bool hasIdx = srcSel != 2;
    uint32_t reset = g_regs[R_RESET_INDX];
    bool resetOn = (mode >> 21) & 1;
    uint32_t minI = 0xFFFFFFFFu, maxI = 0;
    if (hasIdx) {
        idx.resize(num);
        uint32_t endian = dmaSize >> 30;
        if (srcSel == 0) {
            const uint8_t* src = phys(idx32 ? (dmaBase & ~3u) : (dmaBase & ~1u));
            if (idx32) for (uint32_t i = 0; i < num; i++) {
                uint32_t v = ((const uint32_t*)src)[i];
                if (endian == 1) v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
                else if (endian == 2) v = _byteswap_ulong(v);
                else if (endian == 3) v = (v >> 16) | (v << 16);
                idx[i] = v;
            } else for (uint32_t i = 0; i < num; i++) {
                uint16_t v = ((const uint16_t*)src)[i];
                if (endian == 1) v = _byteswap_ushort(v);
                idx[i] = v;
            }
        } else {
            if (!inlineIdx) return;
            if (idx32) memcpy(idx.data(), inlineIdx, num * 4);
            else for (uint32_t i = 0; i < num; i++) idx[i] = (inlineIdx[i / 2] >> ((i & 1) ? 16 : 0)) & 0xFFFF;
        }
        uint32_t rv = idx32 ? reset : (reset & 0xFFFF);
        for (uint32_t i = 0; i < num; i++) {
            if (resetOn && idx[i] == rv) { idx[i] = 0xFFFFFFFFu; continue; }
            minI = std::min(minI, idx[i]); maxI = std::max(maxI, idx[i]);
        }
    } else { minI = 0; maxI = num - 1; }
    if (minI > maxI) return;

    // ---- topology / conversion to something D3D11 can draw ----
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool rect = false;
    std::vector<uint32_t> conv;
    auto vert = [&](uint32_t i) { return hasIdx ? idx[i] : i; };
    switch (prim) {
      case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
      case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
      case 3: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
      case 4: case 7: break;
      case 6: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
      case 8: rect = true; break;
      case 5: case 15: for (uint32_t i = 2; i < num; i++) { conv.push_back(vert(0)); conv.push_back(vert(i - 1)); conv.push_back(vert(i)); } break;
      case 13: for (uint32_t i = 0; i + 4 <= num; i += 4) { uint32_t a = vert(i), b = vert(i + 1), c = vert(i + 2), d = vert(i + 3); conv.insert(conv.end(), { a, b, c, a, c, d }); } break;
      case 14: for (uint32_t i = 0; i + 4 <= num; i += 2) { uint32_t a = vert(i), b = vert(i + 1), c = vert(i + 2), d = vert(i + 3); conv.insert(conv.end(), { a, b, d, a, d, c }); } break;
      case 12: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; for (uint32_t i = 0; i < num; i++) conv.push_back(vert(i)); conv.push_back(vert(0)); break;
      default: logf_once(S("prim%u", prim).c_str(), "unsupported primitive type %u", prim); return;
    }
    if (mx) { topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; rect = false; conv.clear(); }   // each vertex once
    const std::vector<uint32_t>* useIdx = !conv.empty() ? &conv : (hasIdx ? &idx : nullptr);
    uint32_t drawCount = useIdx ? (uint32_t)useIdx->size() : num;
    if (drawCount == 0) return;

    {   // experiment: BCR2_DRAWDELAYUS=n waits n microseconds before each draw reads guest data
        static const int kDelayUs = getenv("ASURA_DRAWDELAYUS") ? atoi(getenv("ASURA_DRAWDELAYUS")) : 0;
        if (kDelayUs > 0) {
            LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&f);
            do { YieldProcessor(); QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < kDelayUs);
        }
        // experiment: BCR2_DRAWSYNC=1 flushes before each draw, =2 also waits for the host GPU to finish everything so far
        static const int kSync = getenv("ASURA_DRAWSYNC") ? atoi(getenv("ASURA_DRAWSYNC")) : 0;
        if (kSync == 1) g_ctx->Flush();
        if (kSync == 2) {
            static ComPtr<ID3D11Query> q;
            if (!q) { D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT; g_dev->CreateQuery(&qd, &q); }
            g_ctx->End(q.Get());
            BOOL done = FALSE;
            while (g_ctx->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
        }
    }
    DP(3);
    // ---- vertex data ----
    int32_t indexOffset = (int32_t)(g_regs[R_INDX_OFFSET] & 0xFFFFFF);
    indexOffset = sext(indexOffset, 24);
    static StateCb cb;
    memset(cb.vfo, 0, sizeof(cb.vfo));
    // Streams already copied into the current ring buffer this frame are reused: instanced fleets draw the same 1-2 MB stream hundreds of
    // times a frame, and copying (and byte-swapping) it for each draw made the zoom-out at the start of episode 1 run at 20 fps.
    // An entry holds while the ring buffer is the same one, no memexport wrote memory, and a sampled checksum of the guest data still matches.
    struct VbCached { uint32_t off = 0, size = 0, sum = 0; uint64_t gen = ~0ull, frame = ~0ull, epoch = 0; };
    static std::unordered_map<uint64_t, VbCached> vbCache;
    static uint64_t vbCacheFrame = ~0ull;
    if (vbCacheFrame != g_frame) { vbCache.clear(); vbCacheFrame = g_frame; }  // entries only live for their frame
    static const bool kNoVbCache = getenv("ASURA_NOVBCACHE") != nullptr;     // for comparison
    auto vb_sum = [](const uint8_t* p, uint32_t size) {                      // 64 dwords spread over the stream, plus its ends
        uint32_t h = 2166136261u;
        const uint32_t n = size / 4, step = std::max(1u, n / 64);
        for (uint32_t i = 0; i < n; i += step) { uint32_t v; memcpy(&v, p + i * 4, 4); h = (h ^ v) * 16777619u; }
        if (n) { uint32_t v; memcpy(&v, p + (n - 1) * 4, 4); h = (h ^ v) * 16777619u; }
        return h;
    };
    struct VbPlan { uint32_t slot, addr, endian, size; uint64_t first, key; uint32_t sum; const VbCached* hit; };
    VbPlan plan[32]; int nPlan = 0;
    {   // All of a draw's streams must land in the same ring buffer: the draw binds one. vb_alloc moves to the next buffer when the current one
        // is full, and when that happened between two streams, the earlier stream was read from the new buffer at its old offset: one draw in one
        // frame got another mesh's UVs or normals (faces and ships flickering to other surfaces, differently each run). Reserve the room first.
        uint64_t missBytes = 0;
        for (auto& u : vs->vf) {
            uint32_t f0 = g_regs[R_FETCH + u.slot * 2], f1 = g_regs[R_FETCH + u.slot * 2 + 1];
            if ((f0 & 3) != 3) { logf_once("vfbad", "vertex fetch constant %u has type %u", u.slot, f0 & 3); continue; }
            uint32_t addr = f0 & ~3u, endian = f1 & 3, words = (f1 >> 2) & 0xFFFFFF;
            uint32_t strideB = u.stride * 4;
            uint64_t first = strideB ? (uint64_t)std::max<int64_t>(0, (int64_t)minI + indexOffset) * strideB : 0;
            uint64_t last = strideB ? ((uint64_t)((int64_t)maxI + indexOffset) + 1) * strideB : (uint64_t)words * 4;
            uint64_t avail = (uint64_t)words * 4;
            if (last > avail) last = avail;
            if (first >= last) { first = 0; last = std::min<uint64_t>(avail, 4096); }
            // Upload the whole stream: the index buffer doesn't say which vertices a shader reads. Instanced meshes compute vertex = index mod instance size
            // in the vertex shader (vs 2090BFF4A9951B2D), so instance 5's indices are large while it reads the first vertices of the stream. A range taken from
            // the indices missed those, and the shader read whatever sat nearby in the ring: stray black geometry and heavy flicker, even with a still camera.
            // BCR2_VBRANGE restores the narrow upload, for comparison.
            static const bool kVbRange = getenv("ASURA_VBRANGE") != nullptr;
            if (!kVbRange) { first = 0; last = avail; }
            uint32_t size = (uint32_t)std::min<uint64_t>(last - first + 64, kVbRingSize / 2);
            if ((first + size) > avail) size = (uint32_t)(avail - first);
            if (size == 0 || nPlan >= 32) continue;
            VbPlan& p = plan[nPlan++];
            p.slot = u.slot; p.addr = addr; p.endian = endian; p.size = size; p.first = first;
            p.key = ((uint64_t)(addr + (uint32_t)first) << 32) ^ ((uint64_t)size << 2) ^ endian;
            p.sum = kNoVbCache ? 0 : vb_sum(phys(addr + (uint32_t)first), size);
            p.hit = nullptr;
            if (!kNoVbCache) {
                auto it = vbCache.find(p.key);
                if (it != vbCache.end() && it->second.gen == g_vbGen && it->second.frame == g_frame && it->second.epoch == g_mxEpoch && it->second.size == size && it->second.sum == p.sum)
                    p.hit = &it->second;
            }
            if (!p.hit) missBytes += align_up(size, 16);
        }
        if (missBytes && vb_reserve(missBytes)) for (int i = 0; i < nPlan; i++) plan[i].hit = nullptr;   // a fresh buffer: copy everything again
    }
    for (int pi = 0; pi < nPlan; pi++) {
        const VbPlan& p = plan[pi];
        if (p.hit) { cb.vfo[p.slot] = p.hit->off - (uint32_t)p.first; continue; }
        const uint32_t addr = p.addr, endian = p.endian, size = p.size; const uint64_t first = p.first;
        struct { uint32_t slot; } u{ p.slot };
        uint8_t* ptr = nullptr;
        uint32_t off = vb_alloc(size, ptr);
        const uint8_t* src = phys(addr + (uint32_t)first);
        {   // BCR2_VBHASH with a traced frame: one line per vertex stream, to see which streams the game rewrites between frames
            static const bool kVbHash = getenv("ASURA_VBHASH") != nullptr;
            if (kVbHash && g_trace > 1000) fprintf(stderr, "[vbhash] f=%llu d=%llu vs=%016llX slot=%u addr=%08X size=%u hash=%016llX\n", (unsigned long long)g_frame, (unsigned long long)g_drawsFrame, (unsigned long long)vs->hash, u.slot, addr, size, (unsigned long long)fnv64(src, size));
        }
        if (endian == 2) {
            uint32_t n = size / 4;
            const uint32_t* s = (const uint32_t*)src; uint32_t* d = (uint32_t*)ptr;
            for (uint32_t i = 0; i < n; i++) d[i] = _byteswap_ulong(s[i]);
        } else {
            memcpy(ptr, src, size);
            if (endian) swap_block(ptr, size, endian);
        }
        vb_commit(off, size, ptr);
        cb.vfo[u.slot] = off - (uint32_t)first;
        if (!kNoVbCache) { VbCached& e = vbCache[p.key]; e.off = off; e.size = size; e.sum = p.sum; e.gen = g_vbGen; e.frame = g_frame; e.epoch = g_mxEpoch; }
    }

    DP(4);
    // ---- textures and samplers ----
    ID3D11ShaderResourceView* srvs[32] = {};
    ID3D11SamplerState* samps[16] = {};
    for (int s = 0; s < 16; s++) {
        cb.tfp[s * 4] = 1; cb.tfp[s * 4 + 1] = 1; cb.tfp[s * 4 + 2] = 1; cb.tfp[s * 4 + 3] = 0;
    }
    bool ringPs = false;                                                    // binding the pixel shader's textures: noted in the draw ring
    auto bind_tex = [&](const TexUse& u) {
        const uint32_t* tf = &g_regs[R_FETCH + u.slot * 6];
        if ((tf[0] & 3) != 2) return;
        uint32_t fmt = tf[1] & 63, base = tf[1] & 0xFFFFF000u, dim = (tf[5] >> 9) & 3;
        uint32_t w = (tf[2] & 0x1FFF) + 1, h = ((tf[2] >> 13) & 0x1FFF) + 1;
        if (dim == 0) h = 1;
        uint32_t sw = (tf[3] >> 1) & 0xFFF;
        for (int i = 0; i < 4; i++) cb.tfsw[u.slot * 4 + i] = (sw >> (3 * i)) & 7;
        for (int i = 0; i < 4; i++) cb.tfsg[u.slot * 4 + i] = (tf[0] >> (2 + i * 2)) & 3;
        cb.tfp[u.slot * 4 + 2] = (float)std::ldexp(1.0, sext((tf[3] >> 13) & 63, 6));
        cb.tfi[u.slot * 4] = 1.0f / w; cb.tfi[u.slot * 4 + 1] = 1.0f / h;
        cb.tfi[u.slot * 4 + 3] = (fmt >= 24 && fmt <= 26) ? 1.0f : (fmt >= 27 && fmt <= 32) ? 3.0f : 0.0f;   // 16-bit decode in texfix
        ID3D11SamplerState* smp = get_sampler(tf, u);
        samps[u.slot] = smp;
        // a resolve destination?
        Surface* sf = surface_at(base);
        if (sf) {
            uint64_t gh = guest_sample_hash(sf->base, sf->end - sf->base);
            if (gh != sf->guestHash) {                                     // guest wrote new CPU data over it: forget the surface
                fprintf(stderr, "[gfx] surface %X overwritten by guest, dropped\n", sf->base);
                g_surfaces.erase(sf->base); delete sf; sf = nullptr;
            }
        }
        if (sf && u.dim != 3) {
            if (texflip_on()) { FlipVal fv; fv.base = base; fv.w = w; fv.h = h; fv.fmt = fmt; fv.src = 1; texflip_note(u.slot, fv); }
            if (ringPs && g_shotRingSize > 0) draw_ring_tex(u.slot, base, sf->guestHash ^ 0x5F);
            {   // ASURA_UITEXLOG: resolved surfaces sampled, once each per shader pair
                static const bool kUiLog = getenv("ASURA_UITEXLOG") != nullptr;
                static std::set<std::pair<uint64_t, uint64_t>> seenUi;
                if (kUiLog && ringPs && seenUi.insert({ (uint64_t)base << 32 | fmt << 24 | w, (vs->hash ^ (ps ? ps->hash : 0)) }).second)
                    fprintf(stderr, "[uitex] f=%llu vs=%016llX ps=%016llX slot=%u base=%08X fmt=%u %ux%u resolved surface\n", (unsigned long long)g_frame,
                            (unsigned long long)vs->hash, (unsigned long long)(ps ? ps->hash : 0), u.slot, base, fmt, w, h);
            }
            bool wantsRaw = sf->rawSrv && fmt != 22 && fmt != 23;           // depth read as bytes, not as a depth format
            srvs[u.slot] = wantsRaw ? sf->rawSrv.Get() : sf->srv.Get();
            switch (sf->fmt) {                                              // 16-bit decode in texfix follows what the surface holds
              case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM: cb.tfi[u.slot * 4 + 3] = 1.0f; break;
              case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM: cb.tfi[u.slot * 4 + 3] = 2.0f; break;
              case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: cb.tfi[u.slot * 4 + 3] = (fmt == 54 || fmt == 7) ? 0.0f : 3.0f; break;
              default: cb.tfi[u.slot * 4 + 3] = 0.0f; break;
            }
            uint32_t depth = 1;
            if (dim == 2) { w = (tf[2] & 0x7FF) + 1; h = ((tf[2] >> 11) & 0x7FF) + 1; depth = (tf[2] >> 22) + 1; }   // 3D
            else if (dim == 1) depth = ((tf[2] >> 26) & 63) + 1;                                                      // 2D, stacked
            cb.tfp[u.slot * 4] = (float)w / sf->w; cb.tfp[u.slot * 4 + 1] = (float)h / sf->h;
            cb.tfi[u.slot * 4] = 1.0f / w; cb.tfi[u.slot * 4 + 1] = 1.0f / h;            if (u.dim == 2) {                                               // slices one under another, 32-row aligned
                cb.tfp[u.slot * 4 + 3] = (float)depth;
                cb.tfi[u.slot * 4 + 2] = (float)align_up(h, 32) / sf->h;
                logf_once(S("stk%X", base).c_str(), "3D/stacked fetch of resolved surface %X: %ux%ux%u, fetch dim %u", base, w, h, depth, dim);
            }
            return;
        }
        {   // The console fills the channels a format doesn't store: one channel reads as RRRR, two channels as RGGG. D3D gives (R,0,0,1) and (R,G,0,1).
            // Normal maps depend on it: the game reads DXN as x and w, so without this one axis of every normal is a constant 1. BCR2_NOCHANFILL shows the old result.
            static const bool kNoFill = getenv("ASURA_NOCHANFILL") != nullptr;
            int fill = 0;
            switch (fmt) {
              case 2: case 8: case 9: case 24: case 27: case 30: case 39: case 59: fill = 1; break;
              case 10: case 13: case 25: case 28: case 31: case 40: case 49: fill = 2; break;
            }
            if (fill && !kNoFill) for (int i = 0; i < 4; i++) {
                int32_t& sel = cb.tfsw[u.slot * 4 + i];
                if (sel >= 0 && sel < 4) sel = fill == 1 ? 0 : (sel == 0 ? 0 : 1);
            }
        }
        FetchInfo fi{};
        {   static const bool kWide = getenv("ASURA_TEXDBGWIDE") != nullptr;   // raw fetch constants of the 2:1 DXT textures that decode scrambled
            if (kWide && w == 2 * h && w <= 256 && (fmt == 18 || fmt == 20))
                logf_once(S("tfraw%X", base).c_str(), "fetch %X fmt %u %ux%u dim %u: %08X %08X %08X %08X %08X %08X (shader dim %u)", base, fmt, w, h, dim, tf[0], tf[1], tf[2], tf[3], tf[4], tf[5], u.dim);
        }
        fi.base = base; fi.fmt = fmt; fi.w = w; fi.h = h; fi.pitch = ((tf[0] >> 22) & 0x1FF) << 5; fi.endian = (tf[1] >> 6) & 3;
        fi.dim = dim; fi.faces = dim == 3 ? 6 : 1; fi.tiled = tf[0] >> 31; fi.packed = (tf[5] >> 11) & 1;
        if (uint32_t mipPage = tf[5] >> 12) {                               // no mip address means the base image is all there is
            uint32_t sizeMax = 0; while ((2u << sizeMax) <= std::max(w, h)) sizeMax++;
            uint32_t mipMin = std::min((tf[4] >> 2) & 15, sizeMax);
            fi.maxLevel = std::max(std::min((tf[4] >> 6) & 15, sizeMax), mipMin);
            if (fi.maxLevel) fi.mipBase = (mipPage & 0x1FFFF) << 12;
            {   // which textures sit on pages the GPU side treats as E0 (shifted 0x1000), once per texture
                bool phys_page_is_e0(uint32_t p);
                static const bool kE0Log = getenv("ASURA_TEXE0LOG") != nullptr;
                if (kE0Log && (phys_page_is_e0(base) || (fi.maxLevel && phys_page_is_e0(fi.mipBase))))
                    logf_once(S("texe0%X", base).c_str(), "texture %X %ux%u fmt %u on E0-marked pages (base %d, mips %d at %X)", base, w, h, fmt,
                              (int)phys_page_is_e0(base), fi.maxLevel ? (int)phys_page_is_e0(fi.mipBase) : -1, fi.mipBase);
            }
            static const long kMipShift = getenv("ASURA_MIPSHIFT") ? strtol(getenv("ASURA_MIPSHIFT"), nullptr, 0) : 0;   // experiment: move the mip address
            if (fi.maxLevel && kMipShift) fi.mipBase = (uint32_t)((long)fi.mipBase + kMipShift);
        }
        if (dim == 2) { logf_once("tex3d", "3D texture not implemented"); return; }
        if ((u.dim == 3) != (dim == 3)) { logf_once("texdim", "texture dimension mismatch shader=%u fetch=%u", u.dim, dim); return; }
        HostTex* ht = get_texture(fi);
        if (!ht) return;
        if (texflip_on()) { FlipVal fv; fv.base = base; fv.mip = fi.mipBase; fv.w = w; fv.h = h; fv.fmt = fmt; fv.data = ht->hash; texflip_note(u.slot, fv); }
        if (ringPs && g_shotRingSize > 0) draw_ring_tex(u.slot, base, ht->hash);
        // ui: Scaleform's textured pixel shader (vs 4213EBD96F9DECE3, or 8F53E8C836CB1279 for a few), and the two-texture one that burns the
        // gauge's ornament away at the end of a tug of war (vs 911E415726EAF644 / ps 87E53336364C051F, 36 vertices; episode 5, logs\st5d)
        if (ringPs) qte_note_texture(base, ht->hash, fmt, w, h, ps && (ps->hash == 0xDDFEE49D043D817Bull || ps->hash == 0x87E53336364C051Full), ht->white);
        {   // ASURA_UITEXLOG=1: each distinct texture a draw samples, once, with the draw's shaders (to find what the UI text is drawn from)
            static const bool kUiLog = getenv("ASURA_UITEXLOG") != nullptr;
            static std::set<std::pair<uint64_t, uint64_t>> seenUi;
            if (kUiLog && ringPs && seenUi.insert({ (uint64_t)base << 32 | fmt << 24 | w, (vs->hash ^ (ps ? ps->hash : 0)) }).second)
                fprintf(stderr, "[uitex] f=%llu vs=%016llX ps=%016llX slot=%u base=%08X fmt=%u %ux%u tiled=%d hash=%016llX%s\n", (unsigned long long)g_frame,
                        (unsigned long long)vs->hash, (unsigned long long)(ps ? ps->hash : 0), u.slot, base, fmt, w, h, (int)fi.tiled, (unsigned long long)ht->hash, ht->mod ? " (mod)" : "");
        }
        {   // one line per distinct fetch while tracing, to check the mip chain and filter a draw really gets
            static std::set<uint64_t> seenFetch;
            if (g_trace > 1000 && seenFetch.insert(((uint64_t)base << 8) | u.slot).second)
                fprintf(stderr, "[texmip] sign=%u%u%u%u swz=%03X exp=%d slot=%u base=%08X %ux%u fmt=%u mipBase=%08X maxLevel=%u levels=%u packed=%d fetch mag=%u min=%u mip=%u aniso=%u arb=%u lodbias=%d minL=%u maxL=%u shader mag=%d min=%d mip=%d\n",
                        (tf[0] >> 2) & 3, (tf[0] >> 4) & 3, (tf[0] >> 6) & 3, (tf[0] >> 8) & 3, (tf[3] >> 1) & 0xFFF, sext((tf[3] >> 13) & 63, 6),
                        u.slot, base, w, h, fmt, fi.mipBase, fi.maxLevel, ht->levels, (int)fi.packed, (tf[3] >> 19) & 3, (tf[3] >> 21) & 3, (tf[3] >> 23) & 3, (tf[3] >> 25) & 7, (tf[3] >> 28) & 7,
                        sext((tf[4] >> 12) & 0x3FF, 10), (tf[4] >> 2) & 15, (tf[4] >> 6) & 15, u.magf, u.minf, u.mipf);
        }
        ID3D11ShaderResourceView* view = ht->srv.Get();
        if (ht->view != DXGI_FORMAT_UNKNOWN) {                              // every channel fetched signed: read the SNORM view, nothing left for texfix to convert
            DXGI_FORMAT typeless; int ch = 0;
            const DXGI_FORMAT sn = snorm_twin(ht->view, &typeless, &ch);
            bool allSigned = ch > 0;
            for (int i = 0; i < ch; i++) if (cb.tfsg[u.slot * 4 + i] != 1) allSigned = false;
            if (allSigned) {
                if (!ht->srvSigned) {
                    ht->srvSigned = make_srv(ht->tex.Get(), sn, ht->faces);
                    static int nl = 0;
                    if (nl++ < 40) fprintf(stderr, "[snorm] texture %08X fmt %u %ux%u read as signed\n", base, fmt, w, h);
                }
                if (ht->srvSigned) { view = ht->srvSigned.Get(); for (int i = 0; i < ch; i++) cb.tfsg[u.slot * 4 + i] = 0; }
            }
        }
        srvs[u.dim == 3 ? 16 + u.slot : u.slot] = view;
        TexFmt t; tex_fmt(fmt, t);
        if (t.block > 1) { cb.tfp[u.slot * 4] = (float)w / ht->hostW; cb.tfp[u.slot * 4 + 1] = (float)h / ht->hostH; }
    };
    if (texflip_on()) texflip_draw(vs->hash, ps ? ps->hash : 0, vs->vf.empty() ? 0 : (g_regs[R_FETCH + vs->vf[0].slot * 2] & ~3u), srcSel == 0 ? dmaBase : 0, num);
    ringPs = true;
    g_hideDraw = false;
    if (ps) for (auto& u : ps->tex) bind_tex(u);
    ringPs = false;
    {   // ASURA_MASHTEXLOG=1 (debug): each shader pair once outside a tug of war and once during one (untextured UI pieces too)
        static const bool kLog = getenv("ASURA_MASHTEXLOG") != nullptr;
        bool story_mash_recent();
        if (kLog) {
            static std::set<uint64_t> seenOut, seenIn;
            const bool in = story_mash_recent();
            const uint64_t key = vs->hash * 31 ^ (ps ? ps->hash : 0);
            if ((in ? seenIn : seenOut).insert(key).second)
                fprintf(stderr, "[mashdraw] f=%llu %s vs=%016llX ps=%016llX tex=%zu num=%u%s%s\n", (unsigned long long)g_frame, in ? "during" : "outside", (unsigned long long)vs->hash,
                        (unsigned long long)(ps ? ps->hash : 0), ps ? ps->tex.size() : 0, num, in && !seenOut.count(key) ? " (only seen during)" : "", g_hideDraw ? " hidden" : "");
        }
    }
    {   // Story Mode's tug of war: Scaleform's untextured fills (vs EC0095A648FC30A5 / ps FA64B15F820AC3EA: the gauges' empty bars, a dark
        // strip across the top otherwise; the user's episode 11 run) go with its textured pieces (qte_note_texture)
        bool story_mash_recent();
        if (ps && vs->hash == 0xEC0095A648FC30A5ull && ps->hash == 0xFA64B15F820AC3EAull && !g_mashBurst && story_mash_recent()) g_hideDraw = true;
    }
    if (g_hideDraw) { g_hideDraw = false; return; }                         // a prompt piece, in Story Mode (qte_note_texture)
    {
        ID3D11ShaderResourceView* vsr[32] = {};
        ID3D11SamplerState* vsm[16] = {};
        ID3D11ShaderResourceView* psKeep[32]; memcpy(psKeep, srvs, sizeof(srvs));
        ID3D11SamplerState* psKeepS[16]; memcpy(psKeepS, samps, sizeof(samps));
        memset(srvs, 0, sizeof(srvs));
        for (auto& u : vs->tex) bind_tex(u);
        memcpy(vsr, srvs, sizeof(srvs)); memcpy(vsm, samps, sizeof(samps));
        g_ctx->VSSetShaderResources(0, 32, vsr);
        ID3D11ShaderResourceView* vbs[1] = { g_vbSrv.Get() };
        g_ctx->VSSetShaderResources(64, 1, vbs);
        g_ctx->VSSetSamplers(0, 16, vsm);
        g_ctx->PSSetShaderResources(0, 32, psKeep);
        g_ctx->PSSetSamplers(0, 16, psKeepS);
    }

    DP(5);
    // ---- viewport mapping into shader constants ----
    uint32_t vte = g_regs[R_VTE_CNTL];
    float xs = (vte & 1) ? reg_f(R_VPORT_XSCALE) : 1, xo = (vte & 2) ? reg_f(R_VPORT_XSCALE + 1) : 0;
    float ys = (vte & 4) ? reg_f(R_VPORT_XSCALE + 2) : 1, yo = (vte & 8) ? reg_f(R_VPORT_XSCALE + 3) : 0;
    float zs = (vte & 16) ? reg_f(R_VPORT_XSCALE + 4) : 1, zo = (vte & 32) ? reg_f(R_VPORT_XSCALE + 5) : 0;
    // A window offset does not move anything on the host: each tile stays at its real screen rows and the resolve reads it from there (host_tile_shift).
    // Tiled depth passes are matched later by an untiled pass with depth EQUAL. Shifted geometry, in the transform or by the viewport, gets slightly
    // different depth, and the lower tile speckled where the test missed.
    // Half a host pixel, not half a guest pixel: at a higher scale the larger shift would leave the first row and column of fullscreen passes unwritten.
    if (!(g_regs[R_VTX_CNTL] & 1)) { const float hp = (g_regs[R_SURFACE_INFO] & 0x3FFF) <= 256 ? 0.5f : 0.5f / g_resScale; xo += hp; yo += hp; }   // small targets: a guest half pixel, as on the console
    cb.vpNdc[0] = xs * 2.0f / rw; cb.vpNdc[1] = -ys * 2.0f / rh; cb.vpNdc[2] = xo * 2.0f / rw - 1.0f; cb.vpNdc[3] = 1.0f - yo * 2.0f / rh;
    cb.vpZ[0] = zs; cb.vpZ[1] = zo; cb.vpZ[2] = (float)((vte >> 8) & 1); cb.vpZ[3] = (float)((vte >> 9) & 1);
    {   // Finishes the half pixel for vertices on half a guest pixel, in the vertex shader. Render scale 1 has nothing left to finish.
        const bool snap = g_resScale > 1 && !(g_regs[R_VTX_CNTL] & 1) && (g_regs[R_SURFACE_INFO] & 0x3FFF) > 256;   // small targets: see vpW.z
        cb.vpSnap[0] = snap ? (float)rw : 0.0f; cb.vpSnap[1] = (float)rh; cb.vpSnap[2] = 0.5f - 0.5f / g_resScale; cb.vpSnap[3] = 0.5f / g_resScale;
    }
    cb.vpW[0] = (float)((vte >> 10) & 1); cb.vpW[1] = (float)indexOffset;
    {   // colour target 0 is the console's 7e3 float format: clamp the output to its range (pixel shader epilogue). ASURA_NO7E3CLAMP to compare.
        static const bool kNo7e3 = getenv("ASURA_NO7E3CLAMP") != nullptr;
        const uint32_t cf = (g_regs[R_COLOR_INFO] >> 16) & 0xF;
        cb.vpW[3] = (!kNo7e3 && !depthOnly && (cf == 3 || cf == 12)) ? 1.0f : 0.0f;
    }
    cb.vpW[2] = (g_resScale > 1 && (g_regs[R_SURFACE_INFO] & 0x3FFF) <= 256) ? 1.0f : 0.0f;   // snap the pixel position on small targets
    uint32_t colorCtl = g_regs[R_COLORCONTROL];
    cb.alpha[0] = (colorCtl & 8) ? (float)(colorCtl & 7) : 7.0f; cb.alpha[1] = reg_f(R_ALPHA_REF); cb.alpha[3] = 0;
    cb.alpha[2] = (!depthOnly && ((g_regs[R_COLOR_INFO] >> 16) & 0xF) == 1) ? 1.0f : 0.0f;
    if (g_trace > 0 && cb.alpha[2] != 0) logf_once("gammart", "gamma colour target in use");
    if (!depthOnly) {   // colour exponent bias and the -32..32 clamp of 16_16(_16_16): see the pixel shader epilogue
        static const bool kNoExpBias = getenv("ASURA_NOEXPBIAS") != nullptr;
        const uint32_t ci = g_regs[R_COLOR_INFO], cf = (ci >> 16) & 0xF;
        const int bias = kNoExpBias ? 0 : sext((ci >> 20) & 63, 6);
        const bool fixed16 = cf == 4 || cf == 5;
        if (bias || fixed16) cb.alpha[3] = (float)std::ldexp(1.0, bias) * (fixed16 ? -1.0f : 1.0f);
        if (bias) logf_once(S("rtexp%d_%u", bias, cf).c_str(), "colour target exponent bias %d on format %u", bias, cf);
    }
    memcpy(cb.bools, &g_regs[R_BOOL], 32);
    memcpy(cb.loops, &g_regs[R_LOOP], 128);
    if (vs->base) { memcpy(cb.vsf, vs->vsf, sizeof(cb.vsf)); memcpy(cb.vsx, vs->vsx, sizeof(cb.vsx)); }   // a generic vertex shader's patched fields
    upload_cb(g_cbState.Get(), &cb, sizeof(cb));
    // experiment: BCR2_FREEZEWIND holds c1.x at the given value for vertex shaders that use it as a wind clock, and only those: other shaders use c1 for other things.
    // BCR2_FREEZEVS is the comma-separated list of those shaders' hashes.
    static const char* kFreeze = getenv("ASURA_FREEZEWIND");
    static const std::string kFreezeVs = getenv("ASURA_FREEZEVS") ? getenv("ASURA_FREEZEVS") : "C5B81BC723BA821F";
    const bool freezeThis = kFreeze && kFreezeVs.find(S("%016llX", (unsigned long long)vs->hash)) != std::string::npos;
    {
        if (freezeThis) {
            uint32_t saved = g_regs[R_CONST + 4]; float f = (float)atof(kFreeze);
            memcpy(&g_regs[R_CONST + 4], &f, 4);
            upload_cb(g_cbFloat.Get(), &g_regs[R_CONST], 8192);
            g_regs[R_CONST + 4] = saved;
            g_constDirty = true;                                            // the next draw gets the real value back
        }
    }
    if (g_constDirty && !freezeThis) { upload_cb(g_cbFloat.Get(), &g_regs[R_CONST], 8192); g_constDirty = false; }
    ID3D11Buffer* cbs[2] = { g_cbFloat.Get(), g_cbState.Get() };
    g_ctx->VSSetConstantBuffers(0, 2, cbs);
    g_ctx->PSSetConstantBuffers(0, 2, cbs);
    g_ctx->VSSetShader(run_sh(vs)->vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(ps ? ps->ps.Get() : nullptr, nullptr, 0);
    g_ctx->GSSetShader(rect ? g_gsRect.Get() : nullptr, nullptr, 0);
    if (rect) g_ctx->GSSetConstantBuffers(0, 0, nullptr);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(topo);

    DP(6);
    // ---- fixed-function ----
    float blendF[4] = { reg_f(R_BLEND_RED), reg_f(R_BLEND_RED + 1), reg_f(R_BLEND_RED + 2), reg_f(R_BLEND_RED + 3) };
    g_ctx->OMSetBlendState(get_blend(mask), blendF, 0xFFFFFFFFu);
    {   // BCR2_DBGADDITIVE: paint additive scene draws solid magenta, to see where they land
        static const bool kDbg = getenv("ASURA_DBGADDITIVE") != nullptr;
        if (kDbg && prim != 6 && ((g_regs[R_BLEND0] >> 8) & 31) == 1) {
            static ComPtr<ID3D11PixelShader> magenta;
            if (!magenta) if (auto b = compile_cached("float4 main(float4 p:SV_Position):SV_Target0 { return float4(1,0,1,1); }\n", "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &magenta);
            g_ctx->PSSetShader(magenta.Get(), nullptr, 0);
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        }
    }
    uint32_t sr = g_regs[R_STENCIL], srbf = g_regs[R_STENCIL_BF];
    uint32_t dcEff = dsv ? dc : 0;
    {   // BCR2_DBGNOZ: no depth test on blended scene draws, to see whether depth is rejecting them
        static const bool kNoZ = getenv("ASURA_DBGNOZ") != nullptr;
        if (kNoZ && prim != 6 && g_regs[R_BLEND0] != 0x00010001u) dcEff &= ~2u;
        // ASURA_NOZPS="hash,hash": no depth test for draws with these pixel shaders, to see whether depth rejects their pixels
        static const char* kNoZPs = getenv("ASURA_NOZPS");
        if (kNoZPs && ps && strstr(kNoZPs, S("%016llX", (unsigned long long)ps->hash).c_str())) dcEff &= ~2u;
    }
    g_ctx->OMSetDepthStencilState(get_ds(dcEff, sr, srbf), sr & 0xFF);
    float polyScale = 0, polyOff = 0;
    // The slope is per pixel, so at a higher render scale the same surface changes less per pixel and the bias has to grow with the scale.
    {   // Front and back faces have their own offsets (PA_SU_POLY_OFFSET_FRONT_* then BACK_*); D3D11 has one, so take the faces this draw keeps.
        // Shadow depth passes drawing back faces only (front culled) set the back offset: using the front one left them unbiased (shadow acne).
        const bool frontOn = (mode >> 11) & 1, backOn = (mode >> 12) & 1, cullFront = (mode & 3) == 1;
        static const bool kFrontOnly = getenv("ASURA_POLYFRONTONLY") != nullptr;
        if (kFrontOnly ? (frontOn || backOn) : (frontOn && !cullFront)) { polyScale = reg_f(R_OFFSET_FRONT_SCALE) / 16.0f * g_resScale; polyOff = reg_f(R_OFFSET_FRONT_SCALE + 1); }
        else if (!kFrontOnly && backOn) { polyScale = reg_f(R_OFFSET_FRONT_SCALE + 2) / 16.0f * g_resScale; polyOff = reg_f(R_OFFSET_FRONT_SCALE + 3); }
        if (g_trace > 0 && (frontOn || backOn))
            fprintf(stderr, "[gfx]   polyoffset mode=%08X front %g,%g back %g,%g -> scale %g offset %g\n", mode, reg_f(R_OFFSET_FRONT_SCALE), reg_f(R_OFFSET_FRONT_SCALE + 1),
                    reg_f(R_OFFSET_FRONT_SCALE + 2), reg_f(R_OFFSET_FRONT_SCALE + 3), polyScale, polyOff);
    }
    bool clipOff = (g_regs[R_CLIP_CNTL] >> 16) & 1;
    {   // ASURA_NOCLIPVS=<vs hash prefix>: draws with that vertex shader get no near/far clipping (depth clamped), to test clipping effects
        static const char* kNc = getenv("ASURA_NOCLIPVS");
        if (kNc && S("%016llX", (unsigned long long)vs->hash).compare(0, strlen(kNc), kNc) == 0) clipOff = true;
    }
    g_ctx->RSSetState(get_rs(mode, rect, clipOff, polyScale, polyOff));
    D3D11_VIEWPORT vp{ 0, 0, (float)(rw * g_resScale), (float)(rh * g_resScale), 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    int sx0, sy0, sx1, sy1; compute_scissor(sx0, sy0, sx1, sy1);
    { int hx, hy; host_tile_shift(hx, hy); sx0 += hx; sx1 += hx; sy0 += hy; sy1 += hy; }
    D3D11_RECT sc{ sx0 * g_resScale, sy0 * g_resScale, std::min(sx1, (int)rw) * g_resScale, std::min(sy1, (int)rh) * g_resScale };
    g_ctx->RSSetScissorRects(1, &sc);

    struct ProbeAfter {                                                     // BCR2_PROBE="x,y": log draws that change this pixel in target 0
        ID3D11Resource* res = nullptr; uint32_t px = 0, py = 0, rw_, rh_; uint64_t before = 0; bool have = false, wide = false;
        bool read(uint64_t& v) {                                            // 8-bit RGBA, or 16-bit float RGBA (the scene's 7e3 targets)
            D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
            if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
            wide = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return false;
            D3D11_BOX box{ px, py, 0, px + 1, py + 1, 1 };
            g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, res, 0, &box);
            D3D11_MAPPED_SUBRESOURCE ms;
            if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
            v = wide ? *(uint64_t*)ms.pData : *(uint32_t*)ms.pData; g_ctx->Unmap(st.Get(), 0);
            return true;
        }
        ProbeAfter(ID3D11RenderTargetView* rtv, uint32_t rw, uint32_t rh, uint32_t pr) : rw_(rw), rh_(rh) {
            static const char* e = getenv("ASURA_PROBE");
            static const char* ef = getenv("ASURA_PROBEF");                  // "u,v" in 0..1 of the draw's scissor, to follow a spot through a blur chain
            if ((!e && !ef) || g_trace <= 1000 || !rtv) return;
            if (ef) {
                float u = 0, v = 0; sscanf(ef, "%f,%f", &u, &v);
                int a, b, c, f; compute_scissor(a, b, c, f);
                px = (uint32_t)(u * (c - a)); py = (uint32_t)(v * (f - b));
            } else sscanf(e, "%u,%u", &px, &py);
            if (px >= rw || py >= rh) return;
            rtv->GetResource(&res);
            have = read(before);
        }
        ~ProbeAfter() {
            if (!res) return;
            uint64_t v;
            auto txt = [&](uint64_t x) {
                if (!wide) return S("%08X", (uint32_t)x);
                auto h = [](uint16_t u) { const uint32_t sgn = (u >> 15) & 1, e = (u >> 10) & 31, m = u & 1023; float f = e == 0 ? m / 1024.0f / 16384.0f : (e == 31 ? 65504.0f : ldexpf(1.0f + m / 1024.0f, (int)e - 15)); return sgn ? -f : f; };
                return S("%.4f,%.4f,%.4f,%.4f", h((uint16_t)x), h((uint16_t)(x >> 16)), h((uint16_t)(x >> 32)), h((uint16_t)(x >> 48)));
            };
            if (have && read(v) && v != before) fprintf(stderr, "[probe] rt=%p at %u,%u %s -> %s (above draw, %ux%u)\n", res, px, py, txt(before).c_str(), txt(v).c_str(), rw_, rh_);
            res->Release();
        }
    } probe(rtvs[0], rw, rh, 0);
    {   // experiment: BCR2_SKIPVS=<hash> leaves out draws using this vertex shader, to test what depends on them
        static const char* kSkip = getenv("ASURA_SKIPVS");
        if (kSkip && S("%016llX", (unsigned long long)vs->hash).compare(0, strlen(kSkip), kSkip) == 0) return;   // a prefix is enough (the draw ring shows the first 8 digits)
    }
    DP(7);
    if (useIdx) {
        uint32_t bytes = drawCount * 4;
        if (bytes > kIbRingSize) { logf_once("ibbig", "index data too large"); return; }
        D3D11_MAP m = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_ibPos + bytes > kIbRingSize) { g_ibPos = 0; m = D3D11_MAP_WRITE_DISCARD; }
        D3D11_MAPPED_SUBRESOURCE ms;
        HRESULT ibHr; { PerfScope t(g_sIb); ibHr = g_ctx->Map(g_ibRing.Get(), 0, m, 0, &ms); }
        if (FAILED(ibHr)) return;
        memcpy((uint8_t*)ms.pData + g_ibPos, useIdx->data(), bytes);
        g_ctx->Unmap(g_ibRing.Get(), 0);
        g_ctx->IASetIndexBuffer(g_ibRing.Get(), DXGI_FORMAT_R32_UINT, g_ibPos);
        g_ibPos += bytes;
        if (mx) mx_bind();
        { PerfScope t(g_sCall); g_ctx->DrawIndexed(drawCount, 0, 0); }
    } else {
        if (mx) mx_bind();
        PerfScope t(g_sCall); g_ctx->Draw(drawCount, 0);
    }
    if (mx) mx_finish(vs->hash, prim, drawCount);
    {   // BCR2_DRAWPRINT=<surface info hex>, with BCR2_PROBEQLIGHT: a fingerprint of each draw to that target, to compare a frame's draws against another frame's
        static const char* kPrint = getenv("ASURA_DRAWPRINT");
        static const uint32_t kPrintSurf = kPrint ? (uint32_t)strtoul(kPrint, nullptr, 16) : 0;
        {   // ASURA_DRAWVERTS in a traced frame (ASURA_DUMPFRAME) on its own: that vertex shader's first stream (up to 64 words) as floats, and c0-c7
            static const char* kV = getenv("ASURA_DRAWVERTS");
            const uint32_t vf0 = g_regs[R_FETCH + 95 * 2] & ~3u, vfw = (g_regs[R_FETCH + 95 * 2 + 1] >> 2) & 0xFFFFFF;
            if (kV && !kPrint && g_trace > 1000 && vf0 && S("%016llX", (unsigned long long)vs->hash) == kV) {
                std::string v = S("[dpverts] f=%llu vf=%08X/%u:", (unsigned long long)g_frame, vf0, vfw);
                for (uint32_t k = 0; k < vfw && k < 64; k++) { uint32_t w = _byteswap_ulong(*(const uint32_t*)phys(vf0 + k * 4)); float f; memcpy(&f, &w, 4); v += S(" %.6g", f); }
                v += " C:";
                for (int k = 0; k < 32; k++) v += S(" %.7g", reg_f(R_CONST + k));
                fprintf(stderr, "%s\n", v.c_str());
            }
        }
        if (kPrint && g_probeQLight && (kPrint[0] == '*' || g_regs[R_SURFACE_INFO] == kPrintSurf)) {
            uint64_t ch = fnv64(&g_regs[R_CONST], 8192), rh = fnv64(&g_regs[0x2000], 0x400 * 4), fh = fnv64(&g_regs[R_FETCH], 0xC0 * 4);
            uint64_t ih = useIdx ? fnv64(useIdx->data(), useIdx->size() * 4) : 0;
            uint32_t vf0 = g_regs[R_FETCH + 95 * 2] & ~3u, vfw = (g_regs[R_FETCH + 95 * 2 + 1] >> 2) & 0xFFFFFF;
            uint64_t vh = vf0 ? fnv64(phys(vf0), vfw * 4) : 0;
            static const char* kVerts = getenv("ASURA_DRAWVERTS");           // vertex shader hash: also print the vertex stream of its draws, as floats
            if (kVerts && vf0 && S("%016llX", (unsigned long long)vs->hash) == kVerts) {
                std::string v = S("[dpverts] f=%llu vf=%08X/%u:", (unsigned long long)g_frame, vf0, vfw);
                for (uint32_t k = 0; k < vfw && k < 128; k++) { uint32_t w = _byteswap_ulong(*(const uint32_t*)phys(vf0 + k * 4)); v += S(" %08X", w); }
                v += " C:";
                for (int k = 0; k < 16; k++) v += S(" %.6g", reg_f(R_CONST + k));
                fprintf(stderr, "%s\n", v.c_str());
            }
            static const char* kFull = getenv("ASURA_DRAWPRINTFULL");        // vertex shader hash: also print every register and shader constant at its draws
            if (kFull && (kFull[0] == '*' || S("%016llX", (unsigned long long)vs->hash) == kFull)) {
                fprintf(stderr, "[dpfull] f=%llu", (unsigned long long)g_frame);
                for (uint32_t r = 0x2000; r < 0x2400; r++) if (g_regs[r]) fprintf(stderr, " r%04X=%08X", r, g_regs[r]);
                for (uint32_t r = 0; r < 512; r++) { const uint32_t* c = &g_regs[R_CONST + r * 4]; if (c[0] | c[1] | c[2] | c[3]) fprintf(stderr, " c%u=%08X,%08X,%08X,%08X", r, c[0], c[1], c[2], c[3]); }
                for (uint32_t r = 0; r < 8; r++) fprintf(stderr, " b%u=%08X", r, g_regs[R_BOOL + r]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[dp] f=%llu vs=%016llX ps=%016llX num=%u vf=%08X/%u idx=%016llX vb=%016llX const=%016llX regs=%016llX fetch=%016llX c204=%08X,%08X,%08X,%08X view=%016llX pos=%.7g,%.7g,%.7g sc=%08X,%08X c212=%g surf=%08X\n", (unsigned long long)g_frame, (unsigned long long)vs->hash, ps ? (unsigned long long)ps->hash : 0ull, drawCount, vf0, vfw, (unsigned long long)ih, (unsigned long long)vh, (unsigned long long)ch, (unsigned long long)rh, (unsigned long long)fh, g_regs[R_CONST + 204 * 4], g_regs[R_CONST + 204 * 4 + 1], g_regs[R_CONST + 204 * 4 + 2], g_regs[R_CONST + 204 * 4 + 3],
                    (unsigned long long)fnv64(&g_regs[R_CONST], 16 * 4), reg_f(R_CONST + 4 * 4 + 3), reg_f(R_CONST + 5 * 4 + 3), reg_f(R_CONST + 6 * 4 + 3), g_regs[0x2081], g_regs[0x2082], reg_f(R_CONST + 212 * 4), g_regs[R_SURFACE_INFO]);
        }
    }
    // BCR2_PROBEQ="x,y": like BCR2_PROBE, but only queues a pixel copy after each draw and reads them at the end of the frame, so nothing forces a sync.
    static const char* kProbeQ = getenv("ASURA_PROBEQ");
    if (kProbeQ && (g_trace > 1000 || g_probeQLight) && rtvs[0]) {
        uint32_t qx = 0, qy = 0; sscanf(kProbeQ, "%u,%u", &qx, &qy);
        ComPtr<ID3D11Resource> res; rtvs[0]->GetResource(&res);
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res.Get())->GetDesc(&d);
        if (qx < d.Width && qy < d.Height && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            d.Width = 1; d.Height = 1; d.MipLevels = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ProbeQ q; q.res = res.Get();
            if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &q.st))) {
                D3D11_BOX box{ qx, qy, 0, qx + 1, qy + 1, 1 };
                g_ctx->CopySubresourceRegion(q.st.Get(), 0, 0, 0, 0, res.Get(), 0, &box);
                const uint32_t* t0 = &g_regs[R_FETCH];
                q.text = S("num=%u prim=%u idx=%d vs=%016llX ps=%016llX blend0=%08X dc=%08X mask=%08X col=%08X surf=%08X vf95=%08X/%08X c0=%g,%g,%g,%g tf0=%08X",
                           drawCount, prim, useIdx ? 1 : 0, (unsigned long long)vs->hash, ps ? (unsigned long long)ps->hash : 0ull, g_regs[R_BLEND0], g_regs[R_DEPTHCONTROL], g_regs[R_COLOR_MASK], g_regs[R_COLOR_INFO], g_regs[R_SURFACE_INFO],
                           g_regs[R_FETCH + 95 * 2], g_regs[R_FETCH + 95 * 2 + 1], reg_f(R_CONST), reg_f(R_CONST + 1), reg_f(R_CONST + 2), reg_f(R_CONST + 3), t0[1]);
                static const char* kLightC = getenv("ASURA_LIGHTC");         // pixel shader hash: add its constants and fetch states to the probe line
                if (kLightC && ps && S("%016llX", (unsigned long long)ps->hash) == kLightC) {
                    q.text += S(" sr=%08X srbf=%08X surfd=%08X", g_regs[R_STENCIL], g_regs[R_STENCIL_BF], g_regs[R_DEPTH_INFO]) + " PC:";
                    for (int k = 256; k < 276; k++) q.text += S(" c%d=%.7g,%.7g,%.7g,%.7g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
                    for (int k = 508; k < 512; k++) q.text += S(" c%d=%.7g,%.7g,%.7g,%.7g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
                    q.text += " VC:";
                    for (int k = 0; k < 12; k++) q.text += S(" c%d=%.7g,%.7g,%.7g,%.7g", k, reg_f(R_CONST + k * 4), reg_f(R_CONST + k * 4 + 1), reg_f(R_CONST + k * 4 + 2), reg_f(R_CONST + k * 4 + 3));
                    q.text += S(" tf0=%08X,%08X,%08X,%08X tf1=%08X,%08X,%08X,%08X", g_regs[R_FETCH], g_regs[R_FETCH + 1], g_regs[R_FETCH + 2], g_regs[R_FETCH + 3], g_regs[R_FETCH + 6], g_regs[R_FETCH + 7], g_regs[R_FETCH + 8], g_regs[R_FETCH + 9]);
                }
                g_probeQ.push_back(std::move(q));
            }
        }
    }
}

static void sprite_pipeline() {
    if (!g_psSprite) {
        const char* ps = "Texture2D src:register(t0); SamplerState smp:register(s0); cbuffer P:register(b2){ float2 ofs; float2 scale; uint swaprb; float gam; float pv; float aa; };\n"
                         "float4 main(float4 p:SV_Position):SV_Target { return src.SampleLevel(smp,(p.xy-ofs)/scale,0); }\n";
        if (auto b = compile_cached(ps, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psSprite);
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blendAlpha);
    }
}
static void draw_sprites(int px, int py, int pw, int ph) {
    std::vector<GpuSprite> list;
    { std::lock_guard<std::mutex> lk(g_spriteLock); list = g_sprites; }
    if (list.empty()) return;
    sprite_pipeline();
    if (!g_psSprite || !g_blendAlpha) return;
    for (const GpuSprite& s : list) {
        ID3D11ShaderResourceView* srv = sprite_srv(s.icon);
        if (!srv) continue;
        const float dx0 = px + s.x / 1280.0f * pw, dy0 = py + s.y / 720.0f * ph, dw = s.w / 1280.0f * pw, dh = s.h / 720.0f * ph;
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        ID3D11RenderTargetView* rtv = g_backRtv.Get();
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        invalidate_binds();
        struct { float ofs[2]; float scale[2]; uint32_t sw; float gam; float pv; float aa; float es, pad[3]; } p{ { dx0, dy0 }, { dw, dh }, 0u, 1.0f, 0.0f, 0.0f, 0.0f, {} };
        upload_cb(g_cbBlit.Get(), &p, sizeof(p));
        ID3D11Buffer* cb = g_cbBlit.Get();
        g_ctx->PSSetConstantBuffers(2, 1, &cb);
        g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_psSprite.Get(), nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->OMSetBlendState(g_blendAlpha.Get(), nullptr, 0xFFFFFFFFu);
        g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
        g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
        D3D11_VIEWPORT vp{ 0, 0, (float)g_backW, (float)g_backH, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc{ (LONG)dx0, (LONG)dy0, (LONG)std::ceil(dx0 + dw), (LONG)std::ceil(dy0 + dh) };
        g_ctx->RSSetScissorRects(1, &sc);
        ID3D11SamplerState* samp = g_presentSampler.Get();
        g_ctx->PSSetSamplers(0, 1, &samp);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, nul);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    }
}

// Pictures the host draws over the frame (options.cpp): the Graphics Options menu, a centred panel, and the FPS counter in the top-left corner.
// Each is premultiplied RGBA that its owner redraws only when it changed (version); here it is uploaded and blended over the back buffer.
bool options_picture(int w, int h, std::vector<uint8_t>& rgba, uint64_t& version);
bool fps_picture(int w, int h, std::vector<uint8_t>& rgba, uint64_t& version);
struct OverlayPic { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; uint64_t version = 0, uploaded = ~0ull; int tw = 0, th = 0; std::vector<uint8_t> px; };
static void draw_overlay(OverlayPic& o, bool (*picture)(int, int, std::vector<uint8_t>&, uint64_t&), int x, int y, int w, int h) {
    static ComPtr<ID3D11BlendState> blendPre;
    w = std::max(w, 1); h = std::max(h, 1);
    if (!picture(w, h, o.px, o.version)) return;                            // nothing to show
    if (!o.tex || o.tw != w || o.th != h) {
        o.tex.Reset(); o.srv.Reset(); o.uploaded = ~0ull;
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &o.tex))) return;
        g_dev->CreateShaderResourceView(o.tex.Get(), nullptr, &o.srv);
        o.tw = w; o.th = h;
    }
    if (o.uploaded != o.version && o.px.size() == (size_t)w * h * 4) { g_ctx->UpdateSubresource(o.tex.Get(), 0, nullptr, o.px.data(), w * 4, 0); o.uploaded = o.version; }
    ComPtr<ID3D11ShaderResourceView>& srv = o.srv;
    sprite_pipeline();
    if (!blendPre) {
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &blendPre);
    }
    if (!g_psSprite || !srv || !blendPre) return;
    const float dx0 = (float)x, dy0 = (float)y;
    ID3D11ShaderResourceView* nul[32] = {};
    g_ctx->PSSetShaderResources(0, 32, nul);
    ID3D11RenderTargetView* rtv = g_backRtv.Get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    invalidate_binds();
    struct { float ofs[2]; float scale[2]; uint32_t sw; float gam; float pv; float aa; float es, pad[3]; } p{ { dx0, dy0 }, { (float)w, (float)h }, 0u, 1.0f, 0.0f, 0.0f, 0.0f, {} };
    upload_cb(g_cbBlit.Get(), &p, sizeof(p));
    ID3D11Buffer* cb = g_cbBlit.Get();
    g_ctx->PSSetConstantBuffers(2, 1, &cb);
    g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_psSprite.Get(), nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->OMSetBlendState(blendPre.Get(), nullptr, 0xFFFFFFFFu);
    g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
    g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
    D3D11_VIEWPORT vp{ 0, 0, (float)g_backW, (float)g_backH, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{ (LONG)dx0, (LONG)dy0, (LONG)(dx0 + w), (LONG)(dy0 + h) };
    g_ctx->RSSetScissorRects(1, &sc);
    ID3D11SamplerState* samp = g_presentSampler.Get();
    g_ctx->PSSetSamplers(0, 1, &samp);
    ID3D11ShaderResourceView* s = srv.Get();
    g_ctx->PSSetShaderResources(0, 1, &s);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 1, nul);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
}
static void draw_options(int px, int py, int pw, int ph) {
    static OverlayPic menu, fps;
    const int mw = pw * 11 / 20, mh = ph * 11 / 20;
    draw_overlay(menu, options_picture, px + (pw - mw) / 2, py + (ph - mh) / 2, mw, mh);
    const int fh = std::max(ph / 24, 12), fw = fh * 6;                       // a small box in the top-left corner of the picture
    draw_overlay(fps, fps_picture, px + fh / 2, py + fh / 2, fw, fh);
}

// The startup screen while shaders compile: "Compiling shaders... NN%", a line saying it happens once, and a progress bar, white on black, centred.
// Drawn with GDI into a premultiplied panel like the Options menu (BCR2 used pictures of the game's own text, which Asura doesn't have).
// pct < 0 clears it.
static int g_progPct = 0;
static bool progress_picture(int w, int h, std::vector<uint8_t>& rgba, uint64_t& version) {
    static int lastPct = -1, lastW = 0, lastH = 0; static uint64_t ver = 1;
    if (g_progPct == lastPct && w == lastW && h == lastH && version == ver) return true;
    ver++; lastPct = g_progPct; lastW = w; lastH = h; version = ver;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT big = CreateFontW(-(h * 30 / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT smallFont = CreateFontW(-(h * 15 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, big);
    const std::wstring line1 = L"Compiling shaders... " + std::to_wstring(std::clamp(g_progPct, 0, 100)) + L"%";
    RECT r1{ 0, 0, w, h * 45 / 100 }; DrawTextW(dc, line1.c_str(), -1, &r1, DT_CENTER | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
    SelectObject(dc, smallFont);
    SetTextColor(dc, RGB(170, 170, 170));
    RECT r2{ 0, h * 47 / 100, w, h * 70 / 100 };
    DrawTextW(dc, L"This happens on the first start (and after an update). The game starts when it is done.", -1, &r2, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // the bar: an outline and the filled part
    const int bx = w / 8, bw = w - 2 * bx, by = h * 80 / 100, bh = std::max(h * 7 / 100, 3);
    HBRUSH dim = CreateSolidBrush(RGB(90, 90, 90)), full = CreateSolidBrush(RGB(235, 235, 235));
    RECT outer{ bx, by, bx + bw, by + bh }; FillRect(dc, &outer, dim);
    RECT fill{ bx, by, bx + bw * std::clamp(g_progPct, 0, 100) / 100, by + bh }; FillRect(dc, &fill, full);
    DeleteObject(dim); DeleteObject(full);
    GdiFlush();
    rgba.resize((size_t)w * h * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) {                            // grey levels as premultiplied white over the black screen
        const uint8_t b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
        rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = std::max(r, std::max(g, b));
    }
    SelectObject(dc, oldFont); DeleteObject(big); DeleteObject(smallFont);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    return true;
}
static void show_compile_progress(int pct) {
    if (!g_swap || !g_backRtv) return;
    static OverlayPic pic;
    g_progShown = pct >= 0;
    if (pct >= 0) g_progPct = std::min(pct, 100);
    const float black[4] = { 0, 0, 0, 1 };
    g_ctx->ClearRenderTargetView(g_backRtv.Get(), black);
    if (pct >= 0) {
        const float fit = std::min(g_backW / 1280.0f, g_backH / 720.0f);
        const int pw = std::max((int)std::lround(1280.0f * fit), 1), ph = std::max((int)std::lround(720.0f * fit), 1);
        const int mw = pw * 3 / 4, mh = ph / 5;
        draw_overlay(pic, progress_picture, ((int)g_backW - mw) / 2, ((int)g_backH - mh) / 2, mw, mh);
    }
    g_swap->Present(0, 0);
    invalidate_binds();
}

// ---- resolve ----
static ID3D11ShaderResourceView* g_blitExtra = nullptr;                        // bound to t2 for the next blit: the gamma screen's sample picture
static float g_blitExpScale = 0;   // resolve blits: 2^(copy exponent bias), 0 = none
static void blit(ID3D11ShaderResourceView* src, ID3D11PixelShader* ps, ID3D11RenderTargetView* dst, uint32_t dw, uint32_t dh,
                 int dx0, int dy0, int dx1, int dy1, int sx, int sy, bool swaprb, float scaleX = 1, float scaleY = 1, ID3D11ShaderResourceView* src2 = nullptr, const float* ofsOverride = nullptr) {
    ID3D11ShaderResourceView* nul[32] = {};
    g_ctx->PSSetShaderResources(0, 32, nul);
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);
    invalidate_binds();
    struct { float ofs[2]; float scale[2]; uint32_t sw; float gam; float pv; float aa; float es, pad[3]; } p{ { (float)(sx - dx0), (float)(sy - dy0) }, { scaleX, scaleY }, swaprb ? 1u : 0u, g_gamma.load(), (g_blitExtra ? 1.0f : 0.0f), (float)g_aaMode.load(), g_blitExpScale, {} };
    if (ofsOverride) { p.ofs[0] = ofsOverride[0]; p.ofs[1] = ofsOverride[1]; }
    upload_cb(g_cbBlit.Get(), &p, sizeof(p));
    ID3D11Buffer* cb = g_cbBlit.Get();
    g_ctx->PSSetConstantBuffers(2, 1, &cb);
    g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
    g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
    D3D11_VIEWPORT vp{ 0, 0, (float)dw, (float)dh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc{ dx0, dy0, dx1, dy1 };
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->PSSetShaderResources(0, 1, &src);
    if (src2) g_ctx->PSSetShaderResources(1, 1, &src2);
    if (g_blitExtra) g_ctx->PSSetShaderResources(2, 1, &g_blitExtra);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 3, nul);
    static const char* probeEnv = getenv("ASURA_PROBE");                     // also report the probed pixel after each resolve blit
    if (probeEnv && g_trace > 1000) {
        uint32_t px = 0, py = 0; sscanf(probeEnv, "%u,%u", &px, &py);
        ID3D11Resource* res = nullptr; dst->GetResource(&res);
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
        if (px < d.Width && py < d.Height && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            d.Width = 1; d.Height = 1; d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> st;
            if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
                D3D11_BOX box{ px, py, 0, px + 1, py + 1, 1 };
                g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, res, 0, &box);
                D3D11_MAPPED_SUBRESOURCE ms;
                if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                    fprintf(stderr, "[probe-blit] dst=%p %ux%u rect=%d,%d-%d,%d src=%d,%d value=%08X\n", res, dw, dh, dx0, dy0, dx1, dy1, sx, sy, *(uint32_t*)ms.pData);
                    g_ctx->Unmap(st.Get(), 0);
                }
            }
        }
        res->Release();
    }
}

// The game restores depth by drawing depth bytes into a colour target at the depth buffer's EDRAM base. Host targets are
// separate, so carry the newer colour contents over when that depth buffer is used next. Stencil restarts at zero.
static void sync_depth_alias(Rt* depth, uint32_t base, uint32_t pitch) {
    for (uint32_t fmt : { 0u, 1u }) {
        uint64_t key = ((uint64_t)base << 32) | ((uint64_t)fmt << 16) | (pitch & 0xFFFF);
        auto it = g_rts.find(key);
        if (it == g_rts.end() || it->second->seq <= depth->seq) continue;
        Rt* c = it->second;
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        g_ctx->OMSetRenderTargets(0, nullptr, depth->dsv.Get());
        invalidate_binds();
        g_ctx->ClearDepthStencilView(depth->dsv.Get(), D3D11_CLEAR_STENCIL, 1.0f, 0);
        g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_psColorToDepth.Get(), nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        g_ctx->OMSetDepthStencilState(get_ds(0x76, 0, 0), 0);               // z on, z write, func always
        g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
        D3D11_VIEWPORT vp{ 0, 0, (float)(depth->w * g_resScale), (float)(depth->h * g_resScale), 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc{ 0, 0, (LONG)(depth->w * g_resScale), (LONG)(depth->h * g_resScale) };
        g_ctx->RSSetScissorRects(1, &sc);
        ID3D11ShaderResourceView* s = c->srv.Get();
        g_ctx->PSSetShaderResources(0, 1, &s);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, nul);
        depth->seq = ++g_rtSeq;
        logf_once("alias", "colour target at EDRAM base %X carried into depth (aliasing)", base);
    }
}

// The 10-bit colour formats share EDRAM bits: 2_10_10_10 (2), its 10_10_10_10 resolve view (10), 2_10_10_10_FLOAT (3, the 7e3
// float format, 3-bit exponent and 7-bit mantissa per channel) and its 16_16_16_16 resolve view (12). UE3 draws its scene as
// format 3, rebinds the same base as format 2 and resolves that, so a later shader can read the raw 7e3 bits and decode them
// itself; until this the format 2 target never got the scene and the whole 3D view came out flat green. Host targets are
// separate per format, so when one of these is bound, the newest one at the same base and pitch is carried over: copied when
// the host formats match, re-encoded between half floats and 7e3 bits when they don't (Xenia's conversion).
static void sync_color_alias(Rt* r, uint32_t base, uint32_t fmt, uint32_t pitch) {
    if (!is_10bit_family(fmt)) return;
    Rt* src = nullptr; uint32_t srcFmt = 0;
    for (uint32_t f : { 2u, 3u, 10u, 12u }) {
        if (f == fmt) continue;
        auto it = g_rts.find(((uint64_t)base << 32) | ((uint64_t)f << 16) | (pitch & 0xFFFF));
        if (it != g_rts.end() && it->second->seq > r->seq && (!src || it->second->seq > src->seq)) { src = it->second; srcFmt = f; }
    }
    if (!src) return;
    if (is_7e3_float(fmt) == is_7e3_float(srcFmt)) {
        g_ctx->CopyResource(r->tex.Get(), src->tex.Get());
    } else {
        static ComPtr<ID3D11PixelShader> toBits, toFloat;
        if (!toBits) {
            const char* common =
                "Texture2D<float4> S : register(t0);\n"
                "uint to7e3(float f) { uint u = asuint(clamp(f, 0.0, 31.875)); uint b;\n"
                "  if (u < 0x3E800000u) { uint sh = min(125u - (u >> 23), 24u); b = ((u & 0x7FFFFFu) | 0x800000u) >> sh; } else b = u + 0xC2000000u;\n"
                "  return ((b + 0x7FFFu + ((b >> 16) & 1u)) >> 16) & 0x3FFu; }\n"
                "float from7e3(uint v) { uint m = v & 0x7Fu, e = v >> 7; return e == 0 ? m / 512.0 : (1.0 + m / 128.0) * exp2((float)e - 3.0); }\n";
            std::string a = std::string(common) + "float4 main(float4 p : SV_Position) : SV_Target0 { float4 c = S.Load(int3(p.xy, 0));\n"
                "  return float4(to7e3(c.r) / 1023.0, to7e3(c.g) / 1023.0, to7e3(c.b) / 1023.0, saturate(c.a)); }\n";
            std::string b = std::string(common) + "float4 main(float4 p : SV_Position) : SV_Target0 { float4 c = S.Load(int3(p.xy, 0));\n"
                "  uint3 v = (uint3)round(saturate(c.rgb) * 1023.0); return float4(from7e3(v.r), from7e3(v.g), from7e3(v.b), c.a); }\n";
            if (auto bl = compile_cached(a, "main", "ps_5_0")) g_dev->CreatePixelShader(bl->GetBufferPointer(), bl->GetBufferSize(), nullptr, &toBits);
            if (auto bl = compile_cached(b, "main", "ps_5_0")) g_dev->CreatePixelShader(bl->GetBufferPointer(), bl->GetBufferSize(), nullptr, &toFloat);
        }
        ID3D11PixelShader* ps = is_7e3_float(srcFmt) ? toBits.Get() : toFloat.Get();
        if (!ps) return;
        ID3D11ShaderResourceView* nul[32] = {};
        g_ctx->PSSetShaderResources(0, 32, nul);
        ID3D11RenderTargetView* rtv = r->rtv.Get();
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        invalidate_binds();
        g_ctx->VSSetShader(g_vsBlit.Get(), nullptr, 0);
        g_ctx->PSSetShader(ps, nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
        g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
        D3D11_VIEWPORT vp{ 0, 0, (float)(r->w * g_resScale), (float)(r->h * g_resScale), 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        D3D11_RECT sc{ 0, 0, (LONG)(r->w * g_resScale), (LONG)(r->h * g_resScale) };
        g_ctx->RSSetScissorRects(1, &sc);
        ID3D11ShaderResourceView* s = src->srv.Get();
        g_ctx->PSSetShaderResources(0, 1, &s);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, nul);
        ID3D11RenderTargetView* none[1] = { nullptr };
        g_ctx->OMSetRenderTargets(1, none, nullptr);
    }
    r->seq = src->seq;
    logf_once(S("alias10_%u_%u", srcFmt, fmt).c_str(), "EDRAM base %X: format %u carried into format %u (shared 10-bit bits)", base, srcFmt, fmt);
}

static void do_resolve() {
    if (kGpuProf) gpuprof_mark("resolve");
    g_resolves++;
    uint32_t cc = g_regs[R_COPY_CONTROL], surf = g_regs[R_SURFACE_INFO], pitch = surf & 0x3FFF;
    uint32_t srcSel = cc & 7, cmd = (cc >> 20) & 3;
    bool colorClear = (cc >> 8) & 1, depthClear = (cc >> 9) & 1, isDepth = srcSel >= 4;
    const uint32_t* vf = &g_regs[R_FETCH];
    if ((vf[0] & 3) != 3) { logf_once("resvf", "resolve without vertex fetch 0"); return; }
    uint32_t vaddr = vf[0] & ~3u, endian = vf[1] & 3;
    float v[6];
    for (int i = 0; i < 6; i++) {
        uint32_t raw = *(const uint32_t*)phys(vaddr + i * 4);
        if (endian == 2) raw = _byteswap_ulong(raw);
        else if (endian == 1) raw = ((raw & 0xFF00FF00u) >> 8) | ((raw & 0x00FF00FFu) << 8);
        else if (endian == 3) raw = (raw >> 16) | (raw << 16);
        memcpy(&v[i], &raw, 4);
    }
    float half = (g_regs[R_VTX_CNTL] & 1) ? 0.0f : 0.5f;
    auto px = [&](float f) { return ((int)std::floor((f + half) * 256.0f + 0.5f) + 127) >> 8; };
    int x0 = std::min({ px(v[0]), px(v[2]), px(v[4]) }), x1 = std::max({ px(v[0]), px(v[2]), px(v[4]) });
    int y0 = std::min({ px(v[1]), px(v[3]), px(v[5]) }), y1 = std::max({ px(v[1]), px(v[3]), px(v[5]) });
    uint32_t mode = g_regs[R_MODE_CNTL];
    if ((mode >> 16) & 1) {
        uint32_t wo = g_regs[R_WINDOW_OFFSET];
        int ox = sext(wo & 0x7FFF, 15), oy = sext((wo >> 16) & 0x7FFF, 15);
        x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    }
    int sx0, sy0, sx1, sy1; compute_scissor(sx0, sy0, sx1, sy1);
    x0 = std::clamp(x0, sx0, sx1); x1 = std::clamp(x1, sx0, sx1); y0 = std::clamp(y0, sy0, sy1); y1 = std::clamp(y1, sy0, sy1);
    x0 &= ~7; y0 &= ~7; x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
    int pa = (int)(pitch & ~7u);
    x1 = std::min(x1, pa); x0 = std::min(x0, pa);
    if (x1 <= x0 || y1 <= y0) return;
    int hostX, hostY; host_tile_shift(hostX, hostY);

    uint32_t destBase = g_regs[R_COPY_DEST_BASE] & 0x1FFFFFFFu, destPitch = g_regs[R_COPY_DEST_PITCH] & 0x3FFF, destH = (g_regs[R_COPY_DEST_PITCH] >> 16) & 0x3FFF;
    uint32_t di = g_regs[R_COPY_DEST_INFO];
    bool destSwap = (di >> 24) & 1;
    uint32_t destFmt = (di >> 7) & 63;
    static const uint32_t regsC[4] = { R_COLOR_INFO, R_COLOR1_INFO, R_COLOR2_INFO, R_COLOR3_INFO };
    uint32_t ci = g_regs[regsC[srcSel & 3]];
    Rt* colorSrc = (!isDepth || colorClear) ? get_color_rt(ci & 0xFFF, (ci >> 16) & 0xF, pitch) : nullptr;
    if (colorSrc) sync_color_alias(colorSrc, ci & 0xFFF, (ci >> 16) & 0xF, pitch);
    Rt* depthRt = (isDepth || depthClear) ? get_depth_rt(g_regs[R_DEPTH_INFO] & 0xFFF, pitch) : nullptr;

    static int nlog = 0;
    if (nlog++ < 60 || g_trace > 0)
        fprintf(stderr, "[gfx] resolve src=%u cmd=%u rect=%d,%d-%d,%d dest=%X pitch=%u h=%u fmt=%u exp=%d num=%u swap=%d clr=%d/%d win=%08X surf=%08X col=%08X dep=%08X cc=%08X dc=%08X\n",
                srcSel, cmd, x0, y0, x1, y1, destBase, destPitch, destH, destFmt, sext((di >> 16) & 63, 6), (di >> 13) & 7, (int)destSwap, (int)colorClear, (int)depthClear, g_regs[R_WINDOW_OFFSET], surf, ci, g_regs[R_DEPTH_INFO], g_regs[R_COLOR_CLEAR], g_regs[R_DEPTH_CLEAR]);

    if (cmd != 3 && destBase != 0 && destPitch != 0) {
        Rt* src = isDepth ? depthRt : colorSrc;
        DXGI_FORMAT fmt = isDepth ? DXGI_FORMAT_R32_FLOAT : src->fmt;
        // 16-bit destinations: the resolve converts to the destination format and number type (RB_COPY_DEST_INFO 13-15:
        // 0 unorm, 1 snorm, 7 float) after scaling by 2^(exponent bias, bits 16-21). The game copies its 16_16_16_16 post
        // buffers out as float, and as unorm with bias -5 (they were drawn with colour bias +5).
        static const bool kNoResolveConv = getenv("ASURA_NORESOLVECONV") != nullptr;
        const uint32_t destNum = (di >> 13) & 7;
        const int destExp = sext((di >> 16) & 63, 6);
        g_blitExpScale = 0;
        if (!isDepth && !kNoResolveConv) {
            static const DXGI_FORMAT k16[3][3] = {                         // [channels 1/2/4][unorm, snorm, float]
                { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_FLOAT },
                { DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_FLOAT },
                { DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_FLOAT } };
            int ch = -1, nt = destNum == 0 ? 0 : destNum == 1 ? 1 : destNum == 7 ? 2 : -1;
            switch (destFmt) {
              case 24: ch = 0; break; case 25: ch = 1; break; case 26: ch = 2; break;
              case 30: ch = 0; nt = 2; break; case 31: ch = 1; nt = 2; break; case 32: ch = 2; nt = 2; break;
            }
            if (ch >= 0 && nt >= 0) fmt = k16[ch][nt];
            if (destExp) g_blitExpScale = (float)std::ldexp(1.0, destExp);
            if (ch >= 0 || destExp) logf_once(S("rcv%u_%u_%d", destFmt, destNum, destExp).c_str(), "resolve to format %u number %u exponent %d", destFmt, destNum, destExp);
        }
        uint32_t bytesPerPx = 4; { TexFmt t; if (tex_fmt(destFmt, t) && t.block == 1) bytesPerPx = t.bpb; }
        uint32_t bl2 = bytesPerPx == 1 ? 0 : bytesPerPx == 2 ? 1 : bytesPerPx == 4 ? 2 : bytesPerPx == 8 ? 3 : 4;
        uint32_t pitchAligned = align_up(destPitch, 32);
        uint32_t heightAligned = align_up(destH ? destH : (uint32_t)y1, 32);
        uint32_t bytes = pitchAligned * heightAligned * bytesPerPx;
        // The destination base carries the tile's offset. Find the surface this belongs to and turn the offset back into x,y.
        Surface* sf = surface_at(destBase);
        int ox = 0, oy = 0;
        if (sf && (sf->pitch != destPitch || sf->fmt != fmt)) {
            if (sf->base == destBase) sf = nullptr;
            else if (sf->pitch != destPitch) sf = nullptr;
        }
        if (sf && sf->fmt != fmt) sf = nullptr;
        if (sf && sf->base != destBase) {
            uint32_t delta = destBase - sf->base;
            // macro-tile aligned: 32x32 blocks, bytes per macro tile = 32*32*bpp
            uint32_t mt = delta / (1024u * bytesPerPx);
            ox = (int)((mt % (pitchAligned / 32)) * 32); oy = (int)((mt / (pitchAligned / 32)) * 32);
            // A tile of that surface only if it fits inside it. A resolve reaching past its end is a new buffer that happens to start inside an
            // old one (render targets moved after the hot springs in episode 10): writing it into the old surface at an offset left the title
            // screen showing the old surface, a stale frame with the live one pasted in its lower right. ASURA_RESOLVEINOLD=1: the old behaviour.
            static const bool kInOld = getenv("ASURA_RESOLVEINOLD") != nullptr;
            if (!kInOld && (ox + x1 > (int)sf->w || oy + y1 > (int)sf->h)) {
                static int nl = 0;
                if (nl++ < 20) fprintf(stderr, "[gfx] resolve to %X starts inside surface %X (%ux%u) but reaches past it: a new surface\n", destBase, sf->base, sf->w, sf->h);
                sf = nullptr; ox = oy = 0;
            }
        }
        if (!sf) sf = make_surface(destBase, destPitch, std::max(destH ? destH : (uint32_t)y1, 1u), bytes, fmt);
        if (sf) {
            const int rs = g_resScale;
            const int srcX = (x0 + hostX) * rs, srcY = (y0 + hostY) * rs;
            int dx0 = x0 + ox, dy0 = y0 + oy, dx1 = x1 + ox, dy1 = y1 + oy;
            blit(src->srv.Get(), isDepth ? g_psBlitDepth.Get() : g_psBlit.Get(), sf->rtv.Get(), sf->w * rs, sf->h * rs, dx0 * rs, dy0 * rs, std::min(dx1, (int)sf->w) * rs, std::min(dy1, (int)sf->h) * rs, srcX, srcY, destSwap && !isDepth);
            if (isDepth && sf->rawRtv)
                blit(src->srv.Get(), g_psBlitRaw.Get(), sf->rawRtv.Get(), sf->w * rs, sf->h * rs, dx0 * rs, dy0 * rs, std::min(dx1, (int)sf->w) * rs, std::min(dy1, (int)sf->h) * rs, srcX, srcY, false, 1, 1, src->stencilSrv.Get());
            sf->guestHash = guest_sample_hash(sf->base, sf->end - sf->base);
            sf->swapped = destSwap && !isDepth;
            {   // ASURA_DUMPRESOLVE=<hex base>: in the traced frame, write that surface after each resolve into it (a shadow map reused within the frame)
                static const char* kDr = getenv("ASURA_DUMPRESOLVE");
                static int seq = 0;
                if (kDr && g_trace > 0 && sf->base == (uint32_t)strtoul(kDr, nullptr, 16)) dump_surfaces(sf->base, seq++);
            }
        }
        g_blitExpScale = 0;
    }
    if (colorClear && colorSrc) {
        uint32_t cv = g_regs[R_COLOR_CLEAR];
        float col[4] = { ((cv >> 16) & 255) / 255.0f, ((cv >> 8) & 255) / 255.0f, (cv & 255) / 255.0f, (cv >> 24) / 255.0f };
        D3D11_RECT r{ (x0 + hostX) * g_resScale, (y0 + hostY) * g_resScale, (x1 + hostX) * g_resScale, (y1 + hostY) * g_resScale };
        if (g_ctx1) g_ctx1->ClearView(colorSrc->rtv.Get(), col, &r, 1);
        else g_ctx->ClearRenderTargetView(colorSrc->rtv.Get(), col);
    }
    if (depthClear && depthRt) {
        uint32_t dv = g_regs[R_DEPTH_CLEAR];
        ID3D11RenderTargetView* none[1] = { nullptr };
        g_ctx->OMSetRenderTargets(1, none, nullptr);
        invalidate_binds();
        // Only the resolved rectangle, as the colour clear above: a whole-target clear also erased what the game keeps in the rest of
        // the buffer (ASURA_FASTCLEARWHOLE=1 for the old whole clear).
        static const bool kWholeR = getenv("ASURA_FASTCLEARWHOLE") != nullptr;
        const float zc = (float)((dv >> 8) & 0xFFFFFF) / 16777215.0f;
        const LONG W = (LONG)(depthRt->w * g_resScale), H = (LONG)(depthRt->h * g_resScale);
        D3D11_RECT r{ std::max<LONG>((x0 + hostX) * g_resScale, 0), std::max<LONG>((y0 + hostY) * g_resScale, 0),
                      std::min<LONG>((x1 + hostX) * g_resScale, W), std::min<LONG>((y1 + hostY) * g_resScale, H) };
        if (kWholeR || (r.left <= 0 && r.top <= 0 && r.right >= W && r.bottom >= H))
            g_ctx->ClearDepthStencilView(depthRt->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, zc, (UINT8)(dv & 0xFF));
        else if (r.right > r.left && r.bottom > r.top) {
            sync_depth_alias(depthRt, g_regs[R_DEPTH_INFO] & 0xFFF, pitch);
            depth_clear_rects(depthRt, { r }, zc, (UINT8)(dv & 0xFF), true);
            static int nl = 0;
            if (nl++ < 4 || g_trace > 0) fprintf(stderr, "[gfx] resolve depth clear rect %ld,%ld-%ld,%ld of %ldx%ld\n", r.left, r.top, r.right, r.bottom, W, H);
        }
        depthRt->seq = ++g_rtSeq;
    }
}

// ---- present ----
// BCR2_SHOTBURST=n with BCR2_SHOTDIR: dump the n frames after each simulated press, to count frames to the visible response.
void gpu_note_press() {
    static const int kBurst = getenv("ASURA_SHOTBURST") ? atoi(getenv("ASURA_SHOTBURST")) : 0;
    fprintf(stderr, "[lat] press seen by guest at frame=%llu\n", (unsigned long long)g_frame);
    if (kBurst > 0) g_shotBurstLeft = kBurst;
}

static std::atomic<bool> g_presentFollowsDisplay{false};
void gpu_set_display_locked(bool locked) { g_presentFollowsDisplay = locked; }

// The display the window is on, for the vblank thread. Refreshed when the window changes monitor.
static std::mutex g_outputLock;
static ComPtr<IDXGIOutput> g_output;
static void track_output() {
    static HMONITOR last = nullptr;
    HMONITOR m = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (m == last) return;
    ComPtr<IDXGIOutput> o;
    if (FAILED(g_swap->GetContainingOutput(&o))) return;
    last = m;
    std::lock_guard<std::mutex> lk(g_outputLock);
    g_output = o;
}
bool gpu_wait_vblank() {
    ComPtr<IDXGIOutput> o;
    { std::lock_guard<std::mutex> lk(g_outputLock); o = g_output; }
    return o && SUCCEEDED(o->WaitForVBlank());
}
// For the display-locked vblank (gpu.cpp): only with vsync on a fixed-refresh display (VRR has its own even cadence; pulldown its own rate).
bool gpu_vblank_lock_wanted() { return g_vsync.load() && !g_tearing && g_pulldownVblank.load() == 0; }
// The display's exact refresh rate (DWM's composition rate, e.g. 59.952), checked every 2 s; 0 if unknown.
double gpu_display_hz() {
    static std::atomic<double> hz{0}; static std::atomic<ULONGLONG> at{0};
    const ULONGLONG now = GetTickCount64();
    if (now - at.load() > 2000) {
        at = now;
        DWM_TIMING_INFO ti{}; ti.cbSize = sizeof ti;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator) hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
    }
    return hz.load();
}

// What reached the screen, as opposed to what was submitted: refresh rate, frames discarded before scanout, and Present-to-scanout time.
static void note_display_stats(LARGE_INTEGER presented, uint64_t frame) {
    static LARGE_INTEGER sent[64]{};
    static UINT lastId = 0; static UINT64 lastRefresh = 0; static LONGLONG lastSync = 0;
    static uint64_t nShown = 0, nSkipped = 0, nRefreshes = 0, nDisjoint = 0, lastLog = 0; static LONGLONG syncTicks = 0;
    static double latSum = 0, latMax = 0;
    UINT id = 0;
    if (SUCCEEDED(g_swap->GetLastPresentCount(&id))) sent[id & 63] = presented;
    DXGI_FRAME_STATISTICS fs{};
    HRESULT hr = g_swap->GetFrameStatistics(&fs);
    if (FAILED(hr)) nDisjoint++;
    else if (fs.PresentCount != lastId) {
        if (lastId) {
            UINT adv = fs.PresentCount - lastId;
            nShown++; nSkipped += adv - 1;
            nRefreshes += (uint64_t)(fs.SyncRefreshCount - lastRefresh);
            syncTicks += fs.SyncQPCTime.QuadPart - lastSync;
            if (sent[fs.PresentCount & 63].QuadPart) {
                double ms = (double)(fs.SyncQPCTime.QuadPart - sent[fs.PresentCount & 63].QuadPart) * g_perfTicksToMs;
                latSum += ms; latMax = std::max(latMax, ms);
            }
        }
        lastId = fs.PresentCount; lastRefresh = fs.SyncRefreshCount; lastSync = fs.SyncQPCTime.QuadPart;
    }
    if (frame - lastLog >= 300) {
        double secs = (double)syncTicks * g_perfTicksToMs / 1000.0;
        // Pulldown: the display's rate as measured over the run (59.952 Hz here where Windows reports 59) sets the game's vblank, so the
        // game's 24 fps stays locked to the display's 3, 2 cadence. Only stretches with nothing discarded count.
        if (g_pulldownVblank.load() > 0 && secs > 5 && nSkipped == 0 && nDisjoint == 0) {
            static double sumRef = 0, sumSecs = 0;
            sumRef += (double)nRefreshes; sumSecs += secs;
            const double hz = sumRef / sumSecs;
            if (hz > 20) g_pulldownVblank = 48.0 * hz / std::round(hz);
        }
        fprintf(stderr, "[disp] frame=%llu presents=%llu shown=%llu discarded=%llu refreshes=%llu hz=%.3f present_to_scan avg=%.1f max=%.1f failed=%llu\n",
                (unsigned long long)frame, (unsigned long long)(frame - lastLog), (unsigned long long)nShown, (unsigned long long)nSkipped, (unsigned long long)nRefreshes,
                secs > 0 ? nRefreshes / secs : 0.0, nShown ? latSum / nShown : 0.0, latMax, (unsigned long long)nDisjoint);
        lastLog = frame; nShown = nSkipped = nRefreshes = nDisjoint = 0; syncTicks = 0; latSum = latMax = 0;
    }
}

// One back buffer pixel as 8-bit r, g, b: the swap chain is 10-bit (R10G10B10A2) unless ASURA_BACKBUFFER8 is set.
static inline void back_px(const uint8_t* s, DXGI_FORMAT f, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (f == DXGI_FORMAT_R10G10B10A2_UNORM) { uint32_t w; memcpy(&w, s, 4); r = (uint8_t)((w >> 2) & 255); g = (uint8_t)((w >> 12) & 255); b = (uint8_t)((w >> 22) & 255); }
    else if (f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { b = s[0]; g = s[1]; r = s[2]; }
    else { r = s[0]; g = s[1]; b = s[2]; }
}
static void save_shot(ID3D11Texture2D* st, const D3D11_TEXTURE2D_DESC& d, uint64_t frame, const char* dir) {
    D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) return;
    std::string p = std::string(dir) + S("\\shot_%06llu.bmp", (unsigned long long)frame);
    if (FILE* f = fopen(p.c_str(), "wb")) {
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        uint32_t imgSize = d.Width * d.Height * 4;
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + imgSize;
        ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        std::vector<uint8_t> row(d.Width * 4);
        for (uint32_t y = 0; y < d.Height; y++) {
            const uint8_t* s = (const uint8_t*)ms.pData + y * ms.RowPitch;
            for (uint32_t x = 0; x < d.Width; x++) { uint8_t r, g, b; back_px(s + x * 4, d.Format, r, g, b); row[x * 4] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255; }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    g_ctx->Unmap(st, 0);
}
// BCR2_SHOTRING=n with BCR2_SHOTDIR: keep the last n images, every frame, in memory. F9 saves them ASURA_SHOTRINGAFTER frames later (default 45),
// so a one-frame glitch seen just before the press is inside.
// BCR2_SHOTRINGAT=frame does the same as pressing F9 at that frame, for testing.
// Where F9 captures go: BCR2_SHOTDIR if set, else a new folder _debug_capture\f9_<frame> beside the exe.
static std::string capture_session_dir(uint64_t frame) {
    if (const char* e = getenv("ASURA_SHOTDIR")) { CreateDirectoryA(e, nullptr); return e; }   // a missing folder silently lost a whole capture
    const std::string base = exe_dir() + "\\_debug_capture";
    CreateDirectoryA(base.c_str(), nullptr);
    const std::string d = base + S("\\f9_%llu", (unsigned long long)frame);
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}
static void shot_ring() {
    static const int kN = g_shotRingSize;
    static const uint64_t kAt = getenv("ASURA_SHOTRINGAT") ? _atoi64(getenv("ASURA_SHOTRINGAT")) : 0;
    static std::vector<ComPtr<ID3D11Texture2D>> tex; static std::vector<uint64_t> frames; static int next = 0; static uint32_t w = 0, h = 0;
    D3D11_TEXTURE2D_DESC d; g_backTex->GetDesc(&d);
    // Above 1920x1080 each kept frame is scaled down to fit 1920x1080 first: at a 4K output 150 full frames were 5 GB of staging memory
    // (9.4 GB for the process against 4.3 without the ring, logs\bb4k_3_ring / bb4k_3_noring) and 1 GB/s of copies.
    // ASURA_SHOTRINGFULL=1 keeps full-size frames.
    static const bool kFull = getenv("ASURA_SHOTRINGFULL") != nullptr;
    const uint32_t bw = d.Width, bh = d.Height;
    const double fit = kFull ? 1.0 : std::min(1.0, std::min(1920.0 / bw, 1080.0 / bh));
    const uint32_t rw = std::max(1u, (uint32_t)(bw * fit + 0.5)), rh = std::max(1u, (uint32_t)(bh * fit + 0.5));
    const bool scaled = rw != bw || rh != bh;
    static ComPtr<ID3D11Texture2D> full, ringSmall; static ComPtr<ID3D11ShaderResourceView> fullSrv; static ComPtr<ID3D11RenderTargetView> smallRtv;
    static ComPtr<ID3D11VertexShader> vs; static ComPtr<ID3D11PixelShader> ps; static ComPtr<ID3D11SamplerState> samp;
    d.Width = rw; d.Height = rh;
    d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (d.Width != w || d.Height != h) {
        tex.clear(); frames.clear(); tex.resize(kN); frames.assign(kN, ~0ull); next = 0; w = d.Width; h = d.Height;
        full.Reset(); fullSrv.Reset(); ringSmall.Reset(); smallRtv.Reset();
        if (scaled) {
            D3D11_TEXTURE2D_DESC f{}; g_backTex->GetDesc(&f); f.BindFlags = D3D11_BIND_SHADER_RESOURCE; f.Usage = D3D11_USAGE_DEFAULT; f.CPUAccessFlags = 0; f.MiscFlags = 0;
            g_dev->CreateTexture2D(&f, nullptr, &full); if (full) g_dev->CreateShaderResourceView(full.Get(), nullptr, &fullSrv);
            f.Width = rw; f.Height = rh; f.BindFlags = D3D11_BIND_RENDER_TARGET;
            g_dev->CreateTexture2D(&f, nullptr, &ringSmall); if (ringSmall) g_dev->CreateRenderTargetView(ringSmall.Get(), nullptr, &smallRtv);
            fprintf(stderr, "[capture] F9 ring keeps frames at %ux%u (output %ux%u)\n", rw, rh, bw, bh);
        }
        if (scaled && !vs) {
            static const char* kVs = "float4 main(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position { uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
            static const char* kPs = "Texture2D t : register(t0); SamplerState s : register(s0); float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.SampleLevel(s, uv, 0); }";
            if (auto b = compile_cached(kVs, "main", "vs_5_0")) g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
            if (auto b = compile_cached(kPs, "main", "ps_5_0")) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
            D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
            g_dev->CreateSamplerState(&sd, &samp);
        }
    }
    if (scaled && (!fullSrv || !smallRtv || !vs || !ps || !samp)) return;
    static uint64_t dumpAt = ~0ull;                                          // F9 keeps recording this long after the press, so what was just seen is inside
    static const uint64_t kAfter = getenv("ASURA_SHOTRINGAFTER") ? _atoi64(getenv("ASURA_SHOTRINGAFTER")) : 45;
    {
        ComPtr<ID3D11Texture2D>& t = tex[next];
        if (!t && FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return;
        if (!scaled) g_ctx->CopyResource(t.Get(), g_backTex.Get());
        else {   // the back buffer can't be sampled: copy it, draw it smaller (bilinear; at 2x, 4 texels into 1), keep that
            g_ctx->CopyResource(full.Get(), g_backTex.Get());
            ID3D11ShaderResourceView* nul[1] = {};
            ID3D11RenderTargetView* rtv = smallRtv.Get();
            g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            invalidate_binds();
            D3D11_VIEWPORT vp{ 0, 0, (float)rw, (float)rh, 0, 1 }; g_ctx->RSSetViewports(1, &vp);
            D3D11_RECT sc{ 0, 0, (LONG)rw, (LONG)rh }; g_ctx->RSSetScissorRects(1, &sc);
            g_ctx->RSSetState(get_rs(0, true, true, 0, 0));
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            g_ctx->OMSetDepthStencilState(get_ds(0, 0, 0), 0);
            g_ctx->IASetInputLayout(nullptr); g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->VSSetShader(vs.Get(), nullptr, 0); g_ctx->GSSetShader(nullptr, nullptr, 0); g_ctx->PSSetShader(ps.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srv = fullSrv.Get(); g_ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState* sp = samp.Get(); g_ctx->PSSetSamplers(0, 1, &sp);
            g_ctx->Draw(3, 0);
            g_ctx->PSSetShaderResources(0, 1, nul);
            g_ctx->CopyResource(t.Get(), ringSmall.Get());
        }
        frames[next] = g_frame;
        next = (next + 1) % kN;
    }
    if ((g_ringDump.exchange(false) || (kAt && g_frame == kAt)) && dumpAt == ~0ull) { dumpAt = g_frame + kAfter; fprintf(stderr, "[capture] F9 at frame %llu: saving at frame %llu\n", (unsigned long long)g_frame, (unsigned long long)dumpAt); }
    if (g_frame == dumpAt) {
        dumpAt = ~0ull;
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        int saved = 0;
        const std::string dirS = capture_session_dir(g_frame); const char* dir = dirS.c_str();
        draw_ring_dump(dir);
        static const bool kNoImg = getenv("ASURA_SHOTRINGNOIMG") != nullptr;   // only the draw list (many captures, little disk)
        if (!kNoImg) for (int i = 0; i < kN; i++) { const int k = (next + i) % kN; if (frames[k] != ~0ull) { save_shot(tex[k].Get(), d, frames[k], dir); saved++; } }
        QueryPerformanceCounter(&b);
        fprintf(stderr, "[capture] frame %llu: saved the previous %d images in %.1f s\n", (unsigned long long)g_frame, saved, (double)(b.QuadPart - a.QuadPart) * g_perfTicksToMs / 1000.0);
        void input_clock_pause(uint64_t ms);
        input_clock_pause((uint64_t)((double)(b.QuadPart - a.QuadPart) * g_perfTicksToMs));   // an input replay waits out the stall
    }
}
// F12: save what is on screen as a PNG in <exe>\..\screenshots. The copy is made here, in the frame; encoding and writing happen on another thread.
static void take_screenshot_if_wanted() {
    static bool f12Prev = false;
    {   // read from the keyboard state too: the window message only arrives while this window has keyboard focus
        const bool d = kbm_reserved_down(VK_F12);
        if (d && !f12Prev) g_screenshotWanted = true;
        f12Prev = d;
    }
    static const uint64_t kAt = getenv("ASURA_F12AT") ? _atoi64(getenv("ASURA_F12AT")) : 0;   // test aid: press F12 at this frame
    if (kAt && g_frame == kAt) g_screenshotWanted = true;
    if (!g_screenshotWanted.exchange(false)) return;
    D3D11_TEXTURE2D_DESC d; g_backTex->GetDesc(&d);
    d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    g_ctx->CopyResource(st.Get(), g_backTex.Get());
    D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    auto px = std::make_shared<std::vector<uint8_t>>((size_t)d.Width * d.Height * 4);
    for (uint32_t y = 0; y < d.Height; y++) {
        const uint8_t* s = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch; uint8_t* o = px->data() + (size_t)y * d.Width * 4;
        for (uint32_t x = 0; x < d.Width; x++) { uint8_t r, g, b; back_px(s + x * 4, d.Format, r, g, b); o[x * 4] = b; o[x * 4 + 1] = g; o[x * 4 + 2] = r; o[x * 4 + 3] = 255; }
    }
    g_ctx->Unmap(st.Get(), 0);
    SYSTEMTIME t; GetLocalTime(&t);
    const std::string dir = bcr2_data_root() + "\\screenshots";
    const std::string path = dir + S("\\bcr2_%04d-%02d-%02d_%02d-%02d-%02d-%03d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const uint32_t w = d.Width, h = d.Height;
    std::thread([px, dir, path, w, h] {
        CreateDirectoryA(dir.c_str(), nullptr);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> f; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICStream> stream; ComPtr<IWICBitmapFrameEncode> frame; bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(std::wstring(path.begin(), path.end()).c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&pf)) && SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)px->size(), px->data())) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
        frame.Reset(); enc.Reset(); stream.Reset(); f.Reset();
        if (SUCCEEDED(co)) CoUninitialize();
        fprintf(stderr, "[screenshot] %s %s\n", ok ? "saved" : "FAILED", path.c_str());
    }).detach();
}
// ASURA_VIDEO=<file.mp4> (video.cpp): every presented frame. The back buffer is copied into one of three staging textures and read
// back two frames later, when the GPU has long finished with it, so recording never waits on the GPU; video.cpp encodes on its own thread.
bool video_wanted();
void video_submit(const uint8_t* rows, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt, uint64_t gameFrame);
static void video_capture() {
    if (!video_wanted() || !g_backTex) return;
    struct Slot { ComPtr<ID3D11Texture2D> tex; uint32_t w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; uint64_t frame = 0; bool full = false; };
    static Slot slots[3];
    static uint64_t n = 0;
    D3D11_TEXTURE2D_DESC d; g_backTex->GetDesc(&d);
    Slot& cur = slots[n % 3];
    if (cur.full) {                                                         // this slot's frame is two presents old: read it out first
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(cur.tex.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
            video_submit((const uint8_t*)ms.pData, cur.w, cur.h, ms.RowPitch, (uint32_t)cur.fmt, cur.frame);
            g_ctx->Unmap(cur.tex.Get(), 0);
        }
        cur.full = false;
    }
    if (!cur.tex || cur.w != d.Width || cur.h != d.Height || cur.fmt != d.Format) {
        D3D11_TEXTURE2D_DESC sd = d; sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
        cur.tex.Reset();
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &cur.tex))) return;
        cur.w = d.Width; cur.h = d.Height; cur.fmt = d.Format;
    }
    g_ctx->CopyResource(cur.tex.Get(), g_backTex.Get());
    cur.frame = g_frame; cur.full = true;
    n++;
}
static void write_shot() {
    video_capture();
    take_screenshot_if_wanted();
    if (!g_logging) return;
    if (g_shotRingSize > 0) shot_ring();
    static const char* envDir = getenv("ASURA_SHOTDIR");                       // set: a frame every BCR2_SHOTEVERY (default 60) goes there as well
    static const int every = envDir ? (getenv("ASURA_SHOTEVERY") ? std::max(atoi(getenv("ASURA_SHOTEVERY")), 1) : 60) : 0;
    static const bool dirMade = envDir ? (CreateDirectoryA(envDir, nullptr), true) : false;   // a missing folder silently loses every shot
    (void)dirMade;
    static std::string burstDir;
    bool burst = g_shotBurstLeft.load() > 0;
    if (burst) { if (burstDir.empty()) burstDir = capture_session_dir(g_frame); g_shotBurstLeft--; if (g_frame % g_shotStep.load() != 0) return; }
    else {   // ASURA_SHOTFROM / ASURA_SHOTTO: only that stretch of frames
        static const uint64_t from = getenv("ASURA_SHOTFROM") ? _strtoui64(getenv("ASURA_SHOTFROM"), nullptr, 10) : 0;
        static const uint64_t to = getenv("ASURA_SHOTTO") ? _strtoui64(getenv("ASURA_SHOTTO"), nullptr, 10) : ~0ull;
        static const std::vector<std::pair<uint64_t, uint64_t>> ranges = [] {   // ASURA_SHOTRANGES="a-b;c-d": several stretches instead
            std::vector<std::pair<uint64_t, uint64_t>> v;
            for (const char* p = getenv("ASURA_SHOTRANGES"); p && *p; ) {
                unsigned long long a = 0, b = 0;
                if (sscanf(p, "%llu-%llu", &a, &b) == 2) v.push_back({a, b});
                p = strchr(p, ';'); if (p) p++;
            }
            return v;
        }();
        bool inRange = ranges.empty() ? (g_frame >= from && g_frame <= to) : false;
        for (auto& r : ranges) if (g_frame >= r.first && g_frame <= r.second) inRange = true;
        {   // ASURA_SHOTAFTERFILE="<file name text>:<from>-<to>": frames counted from when a file with that text in its name is first opened
            // (kfile.cpp), for a cinematic that starts at a different frame in each run (an autopilot run's fights take varying time)
            extern std::atomic<int64_t> g_shotFileFrame;
            static const char* kAf = getenv("ASURA_SHOTAFTERFILE");
            static long long afFrom = 0, afTo = -1;
            static bool parsed = false;
            if (kAf && !parsed) { parsed = true; if (const char* c = strrchr(kAf, ':')) sscanf(c + 1, "%lld-%lld", &afFrom, &afTo); }
            const int64_t at = g_shotFileFrame.load();
            if (kAf && at >= 0) { inRange = (long long)g_frame >= at + afFrom && (long long)g_frame <= at + afTo; }
            else if (kAf) inRange = false;
        }
        burstDir.clear(); if (!every || (g_frame % every) != 0 || !inRange) return;
    }
    const char* dir = burst ? burstDir.c_str() : envDir;
    D3D11_TEXTURE2D_DESC d; g_backTex->GetDesc(&d);
    d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    g_ctx->CopyResource(st.Get(), g_backTex.Get());
    save_shot(st.Get(), d, g_frame, dir);
}

// BCR2_DUMPFRAME=n with BCR2_SHOTDIR: log frame n's draws and resolves, then write each resolved surface as a BMP.
static int64_t g_dumpFrame = (getenv("ASURA_DUMPFRAME") && getenv("ASURA_DUMPFRAME")[0] != '+') ? _atoi64(getenv("ASURA_DUMPFRAME")) : -1;   // also set by F10 (with --log): the next frame
static void dump_surfaces(uint32_t only, int seq) {                          // only/seq: one surface, right after a resolve (ASURA_DUMPRESOLVE)
    const char* dir = getenv("ASURA_SHOTDIR");
    if (!dir) return;
    std::vector<std::pair<Surface*, bool>> all;
    static const char* kOnly = getenv("ASURA_DUMPONLY");                     // hex base address: dump just this surface
    const uint32_t onlyBase = only ? only : kOnly ? (uint32_t)strtoul(kOnly, nullptr, 16) : 0;
    for (auto& kv : g_surfaces) { if (onlyBase && kv.first != onlyBase) continue; all.push_back({ kv.second, false }); if (kv.second->rawTex) all.push_back({ kv.second, true }); }
    for (auto& ent : all) {
        Surface* s = ent.first;
        ID3D11Texture2D* from = ent.second ? s->rawTex.Get() : s->tex.Get();
        D3D11_TEXTURE2D_DESC d; from->GetDesc(&d);
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) continue;
        g_ctx->CopyResource(st.Get(), from);
        D3D11_MAPPED_SUBRESOURCE ms; if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) continue;
        std::string p = std::string(dir) + S("\\surf_%08X_f%d%s_n%llu%s.bmp", s->base, (int)s->fmt, ent.second ? "_raw" : "", (unsigned long long)g_frame, seq >= 0 ? S("_r%d", seq).c_str() : "");
        if (FILE* f = fopen(p.c_str(), "wb")) {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            uint32_t imgSize = d.Width * d.Height * 4;
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + imgSize;
            ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            std::vector<uint8_t> row(d.Width * 4);
            for (uint32_t y = 0; y < d.Height; y++) {
                const uint8_t* src = (const uint8_t*)ms.pData + y * ms.RowPitch;
                auto u8 = [](float v) { return (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f); };
                auto half = [](uint16_t h) {                       // half float to float (denormals flushed; good enough for a picture)
                    const uint32_t e = (h >> 10) & 31, m = h & 1023, sgn = (uint32_t)(h >> 15) << 31;
                    uint32_t b = e == 0 ? sgn : e == 31 ? (sgn | 0x7F800000 | (m << 13)) : (sgn | ((e + 112) << 23) | (m << 13));
                    float f; memcpy(&f, &b, 4); return f; };
                for (uint32_t x = 0; x < d.Width; x++) {
                    uint8_t r, g, b;
                    // Decoded per format, so the picture is the surface's value clamped to 0..1 (half floats and 10-bit used to come out as raw bytes)
                    if (d.Format == DXGI_FORMAT_R32_FLOAT) { float v; memcpy(&v, src + x * 4, 4); r = g = b = u8(v); }
                    else if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) { uint16_t h[4]; memcpy(h, src + x * 8, 8); r = u8(half(h[0])); g = u8(half(h[1])); b = u8(half(h[2])); }
                    else if (d.Format == DXGI_FORMAT_R16G16B16A16_UNORM) { uint16_t h[4]; memcpy(h, src + x * 8, 8); r = h[0] >> 8; g = h[1] >> 8; b = h[2] >> 8; static const bool kA = getenv("ASURA_DUMPALPHA") != nullptr; if (kA) r = g = b = h[3] >> 8; }
                    else if (d.Format == DXGI_FORMAT_R16G16_FLOAT) { uint16_t h[2]; memcpy(h, src + x * 4, 4); r = u8(half(h[0])); g = u8(half(h[1])); b = 0; }
                    else if (d.Format == DXGI_FORMAT_R16G16_UNORM) { uint16_t h[2]; memcpy(h, src + x * 4, 4); r = h[0] >> 8; g = h[1] >> 8; b = 0; }
                    else if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM) { uint32_t w; memcpy(&w, src + x * 4, 4); r = (w >> 2) & 255; g = (w >> 12) & 255; b = (w >> 22) & 255; }
                    else if (d.Format == DXGI_FORMAT_R32G32_FLOAT) { float v[2]; memcpy(v, src + x * 8, 8); r = u8(v[0]); g = u8(v[1]); b = 0; }
                    else if (d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) { float v[4]; memcpy(v, src + x * 16, 16); r = u8(v[0]); g = u8(v[1]); b = u8(v[2]); }
                    else if (d.Format == DXGI_FORMAT_R8G8_UNORM) { r = src[x * 2]; g = src[x * 2 + 1]; b = 0; }
                    else if (d.Format == DXGI_FORMAT_R8_UNORM) { r = g = b = src[x]; }
                    else { r = src[x * 4]; g = src[x * 4 + 1]; b = src[x * 4 + 2]; }
                    row[x * 4] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        g_ctx->Unmap(st.Get(), 0);
    }
    fprintf(stderr, "[gfx] dumped %zu surfaces at frame %llu\n", g_surfaces.size(), (unsigned long long)g_frame);
}

// Between frames: switch display mode if asked to, and match the back buffer to the window.
// Output Resolution (Options, asura.ini outres, ASURA_OUTRES=<height>): in fullscreen the back buffer is that tall instead of the
// display's size, so the present pass and anything sized by the output cost less on a 4K display; the compositor scales it up.
static std::atomic<int> g_outResH{ [] { const int v = getenv("ASURA_OUTRES") ? atoi(getenv("ASURA_OUTRES")) : 0; return v >= 360 && v <= 4320 ? v : 0; }() };
void gpu_set_output_res(int height) { g_outResH = height >= 360 && height <= 4320 ? height : 0; g_sizeChanged = true; fprintf(stderr, "[display] output resolution %s\n", height ? std::to_string(height).c_str() : "native"); }
static void update_display() {
    static WINDOWPLACEMENT savedPlace{ sizeof(WINDOWPLACEMENT) };
    static LONG_PTR savedStyle = 0;
    int want = g_modeWanted.load();
    if (g_modeNow == kExclusive && want == kExclusive) {                    // Alt+Tab or another app can end exclusive mode behind our back
        BOOL fs = FALSE;
        if (SUCCEEDED(g_swap->GetFullscreenState(&fs, nullptr)) && !fs) {
            fprintf(stderr, "[display] exclusive fullscreen was lost, back to a window\n");
            g_modeNow = kWindowed; g_modeWanted = want = kWindowed; g_sizeChanged = true;
        }
    }
    if (want != g_modeNow) {
        if (g_modeNow == kExclusive) g_swap->SetFullscreenState(FALSE, nullptr);
        if (g_modeNow == kBorderless) {
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, savedStyle);
            SetWindowPlacement(g_hwnd, &savedPlace);
            SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        if (want == kBorderless) {
            savedStyle = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
            GetWindowPlacement(g_hwnd, &savedPlace);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        if (want == kExclusive) {
            static const int kOutH = getenv("ASURA_OUTRES") ? atoi(getenv("ASURA_OUTRES")) : 0;
            if (kOutH >= 360 && kOutH <= 4320) { DXGI_MODE_DESC md{}; md.Width = (UINT)((kOutH * 16 + 4) / 9); md.Height = (UINT)kOutH; md.Format = g_backFmt; g_swap->ResizeTarget(&md); }
            HRESULT hr = g_swap->SetFullscreenState(TRUE, nullptr);
            if (FAILED(hr)) { fprintf(stderr, "[display] exclusive fullscreen failed: %08lX\n", hr); g_modeWanted = want = kWindowed; }
        }
        fprintf(stderr, "[display] mode %d -> %d\n", g_modeNow, want);
        g_modeNow = want; g_sizeChanged = true;
    }
    if (!g_sizeChanged.exchange(false)) return;
    RECT rc{}; GetClientRect(g_hwnd, &rc);
    uint32_t w = (uint32_t)(rc.right - rc.left), h = (uint32_t)(rc.bottom - rc.top);
    if (g_modeNow == kBorderless && g_outResH.load() > 0) {                 // Output Resolution: the back buffer at that height and the
        const uint32_t oh = (uint32_t)g_outResH.load();                     // display's shape; the compositor scales it to the screen
        MONITORINFO mi{ sizeof(mi) };                                       // the monitor's shape, not the window's: right after the
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);   // switch the window can still have its old size
        const uint32_t mw = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left), mh = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (mw && mh) { w = (uint32_t)(((uint64_t)oh * mw + mh / 2) / mh) & ~1u; h = oh; }
    }
    {   // ASURA_BACKBUFFER=<w>x<h>: the back buffer at that size whatever the window is (the compositor scales it to the window).
        // For measuring a 4K output on a smaller display; also used when minimised, so a test run pays the full output cost.
        static uint32_t fw = 0, fh = 0;
        static const bool kForce = [] { const char* e = getenv("ASURA_BACKBUFFER"); return e && sscanf(e, "%ux%u", &fw, &fh) == 2 && fw >= 64 && fh >= 64 && fw <= 7680 && fh <= 4320; }();
        if (kForce) { w = fw; h = fh; }
    }
    if (w == 0 || h == 0 || (w == g_backW && h == g_backH)) return;         // minimised, or nothing to do
    ID3D11RenderTargetView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &none, nullptr);
    invalidate_binds();
    g_backRtv.Reset(); g_backTex.Reset();
    g_ctx->Flush();
    HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swapFlags);
    if (FAILED(hr)) fprintf(stderr, "[display] ResizeBuffers %ux%u failed: %08lX\n", w, h, hr);
    else { g_backW = w; g_backH = h; fprintf(stderr, "[display] back buffer %ux%u\n", w, h); }
    g_swap->GetBuffer(0, IID_PPV_ARGS(&g_backTex));
    g_dev->CreateRenderTargetView(g_backTex.Get(), nullptr, &g_backRtv);
}

// BCR2_MEMFIND="frame:HEX,HEX,...": at that frame, list every guest address holding one of these big-endian words, with the words around it.
// Use it to walk from a value seen in a draw back to the game object that holds it, and from there to whatever points at that object.
static void mem_find() {
    // BCR2_MEMSAVE="frame:hexaddr:hexlen:path" writes that guest memory to a file, to compare with what it was loaded from.
    static const char* const kSave = getenv("ASURA_MEMSAVE");
    if (kSave) {
        unsigned long long fr = 0; unsigned addr = 0, len = 0; int used = 0;
        if (sscanf(kSave, "%llu:%x:%x:%n", &fr, &addr, &len, &used) == 3 && used && g_frame == fr) {
            if (FILE* f = fopen(kSave + used, "wb")) { fwrite(g_base + addr, 1, len, f); fclose(f); fprintf(stderr, "[memsave] f=%llu %08X +%X\n", fr, addr, len); }
        }
    }
    static const char* kFind = getenv("ASURA_MEMFIND");
    if (!kFind) return;
    static const uint64_t kFrame = _strtoui64(kFind, nullptr, 10);
    if (g_frame != kFrame) return;
    std::vector<uint32_t> want;
    if (const char* p = strchr(kFind, ':')) {
        while (*p == ':' || *p == ',') { char* e; want.push_back(_byteswap_ulong((uint32_t)strtoul(p + 1, &e, 16))); p = e; }
    }
    uint64_t hits = 0;
    for (uint64_t off = 0; off < 0x100000000ull; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(g_base + off, &mi, sizeof mi)) break;
        const uint64_t size = mi.RegionSize - (uint64_t)((g_base + off) - (uint8_t*)mi.BaseAddress);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uint32_t* w = (const uint32_t*)(g_base + off);
            const uint64_t n = size / 4;
            for (uint64_t i = 0; i < n; i++) {
                for (uint32_t v : want) if (w[i] == v) {
                    if (++hits > 4000) break;
                    std::string s = S("[memfind] f=%llu %08X = %08X :", (unsigned long long)g_frame, (uint32_t)(off + i * 4), _byteswap_ulong(v));
                    for (int64_t k = -12; k <= 12; k++) if ((int64_t)i + k >= 0 && (uint64_t)((int64_t)i + k) < n) s += S(k == 0 ? " [%08X]" : " %08X", _byteswap_ulong(w[i + k]));
                    fprintf(stderr, "%s\n", s.c_str());
                }
            }
        }
        off += size;
    }
    fprintf(stderr, "[memfind] f=%llu done, %llu hits\n", (unsigned long long)g_frame, (unsigned long long)hits);
}

void hw_watch_set(uint32_t phys);
void hw_watch_reads(const uint32_t* a, int n);
uint64_t gpu_frame_number() { return g_frame; }

void funcprof_frame(uint64_t frame);   // funcprof.cpp (profiling build only)
void physwatch_tick();                  // hooks.cpp (ASURA_PHYSWATCH)
static void do_swap(uint32_t front) {
    funcprof_frame(g_frame);
    if (kGpuProf) gpuprof_mark("present / swap");
    physwatch_tick();
    qte_publish();
    {   // ASURA_DUMPONPROMPT=1: trace (as ASURA_DUMPFRAME) the frame after a prompt icon first shows, once
        static const bool kOn = getenv("ASURA_DUMPONPROMPT") != nullptr;
        static bool done = false;
        if (kOn && !done && g_qteActive.load()) { done = true; g_dumpFrame = (int64_t)g_frame + 1; fprintf(stderr, "[qte] tracing frame %lld\n", (long long)g_dumpFrame); }
    }
    {   // ASURA_DUMPONMASH=<n>: trace the frame n frames after a tug-of-war prompt (Story Mode, uescript.cpp story_mash_active) starts, once
        static const char* kOn = getenv("ASURA_DUMPONMASH");
        static int64_t startAt = -1;
        bool story_mash_active();
        if (kOn && startAt < 0 && story_mash_active()) { startAt = (int64_t)g_frame; g_dumpFrame = startAt + atoi(kOn); fprintf(stderr, "[qte] tug of war at %lld: tracing frame %lld\n", (long long)startAt, (long long)g_dumpFrame); }
    }
    {   // ASURA_DUMPFRAME=+<n>: trace the frame n frames after the ASURA_SHOTAFTERFILE file is opened (a cinematic in an autopilot run)
        extern std::atomic<int64_t> g_shotFileFrame;
        static const char* kDf = getenv("ASURA_DUMPFRAME");
        static bool set = false;
        if (kDf && kDf[0] == '+' && !set && g_shotFileFrame.load() >= 0) {
            set = true; g_dumpFrame = g_shotFileFrame.load() + atoi(kDf + 1);
            fprintf(stderr, "[capture] tracing frame %lld (ASURA_DUMPFRAME %s)\n", (long long)g_dumpFrame, kDf);
        }
    }
    { void options_frame(); options_frame(); }                              // Graphics Options menu input (options.cpp)
    LARGE_INTEGER swapStart; QueryPerformanceCounter(&swapStart);
    if (g_drawsLeftOut) {
        fprintf(stderr, "[shader] frame=%llu left out %llu draws waiting on a compile (vs %llu, ps %llu), waited %.1f ms\n", (unsigned long long)g_frame, (unsigned long long)g_drawsLeftOut, (unsigned long long)g_leftOutVs, (unsigned long long)g_leftOutPs, g_compileWaitMs);
        g_drawsLeftOut = g_leftOutVs = g_leftOutPs = 0;
    }
    g_compileWaitMs = 0;
    mem_find();
    {   // BCR2_HWWATCH="frame:physical hex": name the game code that writes this word
        static const char* kHw = getenv("ASURA_HWWATCH");
        if (kHw && g_frame == _strtoui64(kHw, nullptr, 10)) if (const char* p = strchr(kHw, ':')) hw_watch_set((uint32_t)strtoul(p + 1, nullptr, 16));
    }
    {   // BCR2_HWREADS="frame:hex,hex,..": which code reads these guest words, up to four
        static const char* kRd = getenv("ASURA_HWREADS");
        if (kRd && g_frame == _strtoui64(kRd, nullptr, 10)) if (const char* p = strchr(kRd, ':')) {
            uint32_t a[4]; int n = 0; while (n < 4 && (*p == ':' || *p == ',')) { char* e; a[n++] = (uint32_t)strtoul(p + 1, &e, 16); p = e; }
            hw_watch_reads(a, n);
        }
    }
    update_display();
    static const int64_t kDumpCount = getenv("ASURA_DUMPCOUNT") ? _atoi64(getenv("ASURA_DUMPCOUNT")) : 1;   // that many consecutive frames, to compare stages between frames
    static const bool kProbeLight = getenv("ASURA_PROBEQLIGHT") != nullptr;
    if (kProbeLight) {
        const bool closing = g_dumpFrame >= 0 && (int64_t)g_frame >= g_dumpFrame && (int64_t)g_frame < g_dumpFrame + kDumpCount;
        g_probeQLight = g_dumpFrame >= 0 && (int64_t)g_frame + 1 >= g_dumpFrame && (int64_t)g_frame + 1 < g_dumpFrame + kDumpCount;
        if (closing) { probeq_report(); if (getenv("ASURA_DUMPONLY")) dump_surfaces(); fprintf(stderr, "[frame-end] %llu\n", (unsigned long long)g_frame); }
        g_trace = 0;
    } else
    if (g_dumpFrame >= 0 && (int64_t)g_frame >= g_dumpFrame && (int64_t)g_frame < g_dumpFrame + kDumpCount) {
        probeq_report();
        static const bool kNoSurf = getenv("ASURA_DUMPNOSURF") != nullptr;   // pixel traces over many frames don't need the surface files
        if (!kNoSurf) dump_surfaces();
        fprintf(stderr, "[frame-end] %llu\n", (unsigned long long)g_frame);
        if ((int64_t)g_frame == g_dumpFrame + kDumpCount - 1) g_trace = 0;
    }
    if ((int64_t)g_frame + 1 == g_dumpFrame && !kProbeLight) g_trace = 100000;
    uint32_t fp = front & 0x1FFFFFFFu;
    Surface* s = surface_at(fp);
    static int nl = 0;
    if (nl++ < 24) fprintf(stderr, "[gfx] swap front=%08X surface=%s draws=%llu resolves=%llu t=%lu\n", front, s ? "yes" : "no", (unsigned long long)g_drawsFrame, (unsigned long long)g_resolves, GetTickCount());
    if (s) {
        if (s->base != fp) logf_once("frontoff", "front buffer %X inside surface %X", fp, s->base);
        // A swapped resolve holds what guest memory would hold (B in the low byte); scanout undoes that.
        static const bool kNoRamp = getenv("ASURA_NOGAMMARAMP") != nullptr;
        if (g_lutDirty && g_lutTex) {
            uint32_t texel[256];
            for (int i = 0; i < 256; i++) {                                 // register holds red in the high bits, the texture in the low bits
                uint32_t v = g_lutEntries[i];
                texel[i] = ((v >> 20) & 0x3FF) | (((v >> 10) & 0x3FF) << 10) | ((v & 0x3FF) << 20) | (3u << 30);
            }
            g_ctx->UpdateSubresource(g_lutTex.Get(), 0, nullptr, texel, sizeof(texel), 0);
            g_lutDirty = false;
            static int nl = 0;
            if (nl++ < 4) fprintf(stderr, "[gfx] display gamma ramp loaded: in 64 -> %u, 128 -> %u, 192 -> %u, 255 -> %u (of 1023)\n", g_lutEntries[64] & 0x3FF, g_lutEntries[128] & 0x3FF, g_lutEntries[192] & 0x3FF, g_lutEntries[255] & 0x3FF);
        }
        bool useRamp = g_lutSet && !kNoRamp && g_psPresentLut && g_lutSrv;
        // Keep 16:9 in whatever size the back buffer is now, with black bars where it doesn't fit.
        const float fit = std::min(g_backW / 1280.0f, g_backH / 720.0f);
        const int pw = std::max((int)std::lround(1280.0f * fit), 1), ph = std::max((int)std::lround(720.0f * fit), 1);
        const int px = ((int)g_backW - pw) / 2, py = ((int)g_backH - ph) / 2;
        const float black[4] = { 0, 0, 0, 1 };
        if (pw != (int)g_backW || ph != (int)g_backH) g_ctx->ClearRenderTargetView(g_backRtv.Get(), black);
        const float sxs = (float)(s->w * g_resScale) / pw, sys = (float)(s->h * g_resScale) / ph, ofs[2] = { -px * sxs, -py * sys };
        ID3D11SamplerState* presentSamp = g_presentSampler.Get();
        g_ctx->PSSetSamplers(0, 1, &presentSamp);
        g_blitExtra = g_gammaPreview.load() ? gamma_preview_srv() : nullptr;
        blit(s->srv.Get(), useRamp ? g_psPresentLut.Get() : g_psPresentPlain.Get(), g_backRtv.Get(), g_backW, g_backH, px, py, px + pw, py + ph, 0, 0, s->swapped, sxs, sys, useRamp ? g_lutSrv.Get() : nullptr, ofs);
        g_blitExtra = nullptr;
        draw_sprites(px, py, pw, ph);
        draw_options(px, py, pw, ph);
    } else {
        float c[4] = { 0.05f, 0.0f, 0.1f, 1 };
        g_ctx->ClearRenderTargetView(g_backRtv.Get(), c);
    }
    {   // F9 again, read from the keyboard state: the window message only arrives while this window has keyboard focus, and a pad player may not have clicked it
        static const bool kShots = true;
        static bool f9Prev = false;
        if (kShots && g_logging) {
            const bool d = kbm_reserved_down(VK_F9);
            if (d && !f9Prev && g_frame > g_lastF9 + 90) { if (g_shotRingSize > 0) g_ringDump = true; else { g_shotBurstLeft = 180; g_shotStep = 3; } fprintf(stderr, "[capture] F9 pressed (polled) at frame %llu\n", (unsigned long long)g_frame); }
            f9Prev = d;
            // F10: dump the next frame in full (every surface, and the per-draw trace in the log), as ASURA_DUMPFRAME would
            static bool f10Prev = false;
            const bool d10 = kbm_reserved_down(VK_F10);
            if (d10 && !f10Prev) { g_dumpFrame = (int64_t)g_frame + 1; fprintf(stderr, "[capture] F10: dumping frame %lld\n", (long long)g_dumpFrame); }
            f10Prev = d10;
        }
    }
    write_shot();
    note_present_for_fps();
    HRESULT hr;
    LARGE_INTEGER presented; QueryPerformanceCounter(&presented);
    g_swPrep = (double)(presented.QuadPart - swapStart.QuadPart) * g_perfTicksToMs;
    {   // A tick that follows the display can wait for its refresh: nothing piles up, and no frame is discarded when one arrives close to the deadline.
        if (gpu_vrr()) {
            // Hold the frame until a fixed time after its tick, so differences in render time don't reach the screen as uneven frame times.
            static const double kPhaseMs = getenv("ASURA_VRRPHASE") ? atof(getenv("ASURA_VRRPHASE")) : 14.0;
            static uint64_t nLate = 0, nShown = 0; static double phaseSum = 0, phaseMax = 0, holdLateMax = 0;
            LARGE_INTEGER ready; QueryPerformanceCounter(&ready);
            const long long tick = g_lastTickQpc.load();
            const double phase = tick ? (double)(ready.QuadPart - tick) * g_perfTicksToMs : 0;
            phaseSum += phase; phaseMax = std::max(phaseMax, phase); nShown++;
            // Never hold a frame past one frame time since the last present. One that finishes just after the next tick reads as early for that tick,
            // and holding it to that tick's target would turn about 1 ms of lateness into a whole extra frame.
            static long long lastPresent = 0;
            const double sinceLast = lastPresent ? (double)(ready.QuadPart - lastPresent) * g_perfTicksToMs : 1e9;
            double wait = std::min(kPhaseMs - phase, 1000.0 / 60.0 - sinceLast);
            if (!tick) {
                // This host has no game tick to hold to (gpu_note_tick is BCR2's). Release frames on an even cadence instead, one game frame
                // period apart (two vblanks, gpu_frame_period_ms: 33.3 ms at 30 fps, 16.7 at 60, 41.7 at 24), the same clock the game runs
                // on, so the two can't drift. A frame ready after its slot (by more than 0.5 ms) is shown at once and the cadence moves
                // to it, so it settles just after the slowest recent frame and the rest wait for their slot: even frame times on a VRR
                // display, at the cost of a few ms of latency.
                double gpu_frame_period_ms();
                static long long slot = 0;                                  // the next frame's release time (QPC)
                const double period = gpu_frame_period_ms();
                const long long periodQpc = (long long)(period / g_perfTicksToMs);
                if (slot > ready.QuadPart + periodQpc) slot = ready.QuadPart + periodQpc;   // presents faster than the cadence (loading screens):
                                                                                            // never hold one more than a frame period
                const double late = slot ? (double)(ready.QuadPart - slot) * g_perfTicksToMs : 1e9;
                if (late > 0.5) { slot = ready.QuadPart + periodQpc; wait = 0; nLate++; }
                else { wait = late < 0 ? -late : 0; slot += periodQpc; }
                if (wait > 0) {
                    const long long until = ready.QuadPart + (long long)(wait / g_perfTicksToMs);
                    { PerfScope th(g_swHold); gpu_wait_until(until); }
                    LARGE_INTEGER q; QueryPerformanceCounter(&q); holdLateMax = std::max(holdLateMax, (double)(q.QuadPart - until) * g_perfTicksToMs);
                }
                wait = 0;                                                   // handled: skip the tick hold below
            }
            if (!tick) {}
            else if (wait <= 0) nLate++;
            else {
                const long long until = ready.QuadPart + (long long)(wait / g_perfTicksToMs);
                { PerfScope th(g_swHold); gpu_wait_until(until); }
                LARGE_INTEGER q; QueryPerformanceCounter(&q); holdLateMax = std::max(holdLateMax, (double)(q.QuadPart - until) * g_perfTicksToMs);
            }
            QueryPerformanceCounter(&presented);
            lastPresent = presented.QuadPart;
            { PerfScope perfPresent(g_tPresent); hr = g_swap->Present(0, (g_tearing && g_modeNow != kExclusive) ? DXGI_PRESENT_ALLOW_TEARING : 0); }
            if (nShown >= 300) { fprintf(stderr, "[vrr] frame ready after its tick: avg=%.1f max=%.1f ms, shown with no hold (target %.1f ms): %llu of %llu; latest wake: hold %.2f ms, tick %.2f ms\n", phaseSum / nShown, phaseMax, kPhaseMs, (unsigned long long)nLate, (unsigned long long)nShown, holdLateMax, g_tickLateMaxUs.exchange(0) / 1000.0); nShown = nLate = 0; phaseSum = phaseMax = holdLateMax = 0; }
        } else {
        static const int kForce = getenv("ASURA_PRESENTINTERVAL") ? atoi(getenv("ASURA_PRESENTINTERVAL")) : -1;
        UINT interval = kForce >= 0 ? (UINT)kForce : ((g_presentFollowsDisplay.load() || g_vsync.load()) ? 1u : 0u);
        if (kForce < 0) if (const UINT p = pulldown_interval()) interval = p;
        // A stall such as a load can leave one extra frame queued, and at a steady 1:1 rate that adds a full refresh of latency forever.
        // Every Present then blocks, so after 30 of those in a row, leave one frame out to empty the queue.
        static int blocked = 0;
        static const bool kNoDrain = getenv("ASURA_NODRAIN") != nullptr;
        if (interval == 1 && blocked >= 30 && !kNoDrain) {
            static uint64_t nDrain = 0;
            if (nDrain++ < 40) fprintf(stderr, "[disp] frame=%llu queue backed up, left one frame out (#%llu)\n", (unsigned long long)g_frame, (unsigned long long)nDrain);
            blocked = 0;
            hr = S_OK;
        } else {
            PerfScope perfPresent(g_tPresent); hr = g_swap->Present(interval, 0);
            LARGE_INTEGER done; QueryPerformanceCounter(&done);
            blocked = (double)(done.QuadPart - presented.QuadPart) * g_perfTicksToMs > 1.0 ? blocked + 1 : 0;
        }
        }
    }
    {
        PerfScope tt(g_swTrack);
        track_output();
        note_display_stats(presented, g_frame);
    }
    gpuprof_frame();
    {
        static LARGE_INTEGER last{}; static uint64_t lastFrame = 0, lastDraws = 0, lastRes = 0;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        static LARGE_INTEGER prevSwap{}; static std::vector<float> intervals;
        if (prevSwap.QuadPart) intervals.push_back((float)((double)(now.QuadPart - prevSwap.QuadPart) * g_perfTicksToMs));
        {   // one line per late frame, saying where the time went
            static double pD = 0, pT = 0, pR = 0, pP = 0; static uint64_t pW0 = 0, pW1 = 0, pW2 = 0, pCompiles = 0, pUploads = 0, pUpBytes = 0;
            uint64_t w0 = g_waitTicks[0].load(), w1 = g_waitTicks[1].load(), w2 = g_waitTicks[2].load();
            double ms = prevSwap.QuadPart ? (double)(now.QuadPart - prevSwap.QuadPart) * g_perfTicksToMs : 0;
            const double worstDraw = g_worstDrawMs; g_worstDrawMs = 0;
            if (kDrawProf) {   // one long frame vanishes in the 300-frame average: break it down on its own
                static double pDp[10] = {};
                if (ms > 100.0) {
                    std::string s = S("[spikeprof] frame=%llu ms=%.1f", (unsigned long long)g_frame, ms);
                    for (int k = 0; k < 10; k++) if (k != 8) s += S(" %s=%.1f", kDpNames[k], g_dp[k] - pDp[k]);
                    fprintf(stderr, "%s\n", s.c_str());
                }
                memcpy(pDp, g_dp, sizeof pDp);
            }
            if (ms > 20.0 && g_frame > 300) {
                static int nSpike = 0;
                if (nSpike++ < 20000)
                    fprintf(stderr, "[spike] frame=%llu ms=%.1f draw=%.1f tex=%.1f res=%.1f pres=%.1f ring=%.1f reg=%.1f intr=%.1f compiles=%llu uploads=%llu upKB=%llu worst=%.1f vs=%08llX num=%u surf=%08X wcb=%.1f wvb=%.1f wib=%.1f wcall=%.1f wrt=%.1f prep=%.1f hold=%.1f track=%.1f\n",
                            (unsigned long long)g_frame, ms, g_tDraw >= pD ? g_tDraw - pD : g_tDraw, g_tTex >= pT ? g_tTex - pT : g_tTex,
                            g_tResolve >= pR ? g_tResolve - pR : g_tResolve, g_tPresent >= pP ? g_tPresent - pP : g_tPresent,
                            (w0 >= pW0 ? w0 - pW0 : w0) * g_perfTicksToMs, (w1 >= pW1 ? w1 - pW1 : w1) * g_perfTicksToMs, (w2 >= pW2 ? w2 - pW2 : w2) * g_perfTicksToMs,
                            (unsigned long long)(g_nCompiles.load() - pCompiles), (unsigned long long)(g_nUploads - pUploads), (unsigned long long)((g_upBytes - pUpBytes) / 1024), worstDraw, (unsigned long long)g_worstVs, g_worstNum, g_worstSurf, g_wCb, g_wVb, g_wIb, g_wCall, g_wRt, g_swPrep, g_swHold, g_swTrack);
            }
            if (ms > 100.0 && kDrawProf) {                                   // a visible hitch: where the draw time went, by phase
                std::string s = S("[hitch] frame=%llu ms=%.1f draws=%llu", (unsigned long long)g_frame, ms, (unsigned long long)g_drawsFrame);
                for (int k = 0; k < 10; k++) if (k != 8) s += S(" %s=%.1f", kDpNames[k], g_dpFrame[k]);
                fprintf(stderr, "%s\n", s.c_str());
            }
            memset(g_dpFrame, 0, sizeof(g_dpFrame));
            pD = g_tDraw; pT = g_tTex; pR = g_tResolve; pP = g_tPresent; pW0 = w0; pW1 = w1; pW2 = w2; pCompiles = g_nCompiles.load(); pUploads = g_nUploads; pUpBytes = g_upBytes;
        }
        prevSwap = now;
        if (g_frame + 1 - lastFrame >= 300) {
            uint64_t n = g_frame + 1 - lastFrame;
            if (!intervals.empty()) {                                        // pacing matters as much as the average
                std::sort(intervals.begin(), intervals.end());
                size_t c = intervals.size(), late = 0;
                for (float v : intervals) if (v > 20.0f) late++;
                fprintf(stderr, "[pace] frame=%llu p1=%.1f p50=%.1f p99=%.1f max=%.1f over20ms=%zu\n", (unsigned long long)g_frame, intervals[c / 100], intervals[c / 2], intervals[c * 99 / 100], intervals[c - 1], late);
                intervals.clear();
            }
            if (last.QuadPart) {
                double ms = (double)(now.QuadPart - last.QuadPart) * g_perfTicksToMs;
                double w0 = g_waitTicks[0].exchange(0) * g_perfTicksToMs, w1 = g_waitTicks[1].exchange(0) * g_perfTicksToMs, w2 = g_waitTicks[2].exchange(0) * g_perfTicksToMs;
                fprintf(stderr, "[perf] frame=%llu fps=%.1f ms/frame=%.2f draws=%.0f res=%.0f | draw=%.2f (tex=%.2f) resolve=%.2f present=%.2f | wait ring=%.2f reg=%.2f intr=%.2f\n",
                        (unsigned long long)g_frame, n * 1000.0 / ms, ms / n, (double)(g_draws - lastDraws) / n, (double)(g_resolves - lastRes) / n,
                        g_tDraw / n, g_tTex / n, g_tResolve / n, g_tPresent / n, w0 / n, w1 / n, w2 / n);
                drawprof_report(n);
                vram_report();
            }
            last = now; lastFrame = g_frame + 1; lastDraws = g_draws; lastRes = g_resolves;
            g_tDraw = g_tTex = g_tResolve = g_tPresent = 0;
        }
    }
    if (FAILED(hr)) logf_once("present", "Present failed: %08lX", hr);
    // Flip model unbinds the back buffer; keep our view tracking honest.
    invalidate_binds();
    g_frame++;
    g_drawsFrame = 0;
    if ((g_frame % 600) == 0) fprintf(stderr, "[gfx] frame=%llu draws=%llu resolves=%llu\n", (unsigned long long)g_frame, (unsigned long long)g_draws, (unsigned long long)g_resolves);
}

// ASURA_CONSTWATCH=<float constant number>: while a frame is traced (ASURA_DUMPFRAME), log each packet that writes that constant,
// with its kind, the values and (LOAD_ALU_CONSTANT) the memory address it read, between the draw lines.
static void const_watch(const char* kind, uint32_t idx, uint32_t n, const uint32_t* v, uint32_t addr) {
    static const int kC = getenv("ASURA_CONSTWATCH") ? atoi(getenv("ASURA_CONSTWATCH")) : -1;
    if (kC < 0 || g_trace <= 0) return;
    const uint32_t lo = R_CONST + kC * 4;
    if (idx + n <= lo || idx >= lo + 4) return;
    std::string s = S("[constwatch] %s regs %X..%X (c%u..c%u) addr=%08X c%d=", kind, idx, idx + n - 1, (idx - R_CONST) / 4, (idx + n - 1 - R_CONST) / 4, addr, kC);
    for (uint32_t k = 0; k < 4; k++) { const uint32_t r = lo + k; if (r >= idx && r < idx + n) { float f; memcpy(&f, &v[r - idx], 4); s += S("%g%s", f, k < 3 ? "," : ""); } else s += k < 3 ? "-," : "-"; }
    fprintf(stderr, "%s\n", s.c_str());
}
// ---- packets ----
void gpu_packet(uint8_t* base, uint32_t op, const uint32_t* b, uint32_t cnt) {
    static bool inited = false;
    if (!inited) {
        inited = true; g_base = base;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_perfTicksToMs = 1000.0 / (double)f.QuadPart;
        g_devOk = device_init();
        if (g_devOk) shader_precompile();
    }
    if (!g_devOk) return;
    {   // BCR2_PM4TRACE=<surface info hex>, with BCR2_PROBEQLIGHT: every packet while that target is bound, with where it was read from
        static const char* kTrace = getenv("ASURA_PM4TRACE");
        static const uint32_t kTraceSurf = kTrace ? (uint32_t)strtoul(kTrace, nullptr, 16) : 0;
        if (kTrace && g_probeQLight && g_regs[R_SURFACE_INFO] == kTraceSurf) {
            std::string s = S("[pm4] f=%llu at=%08X op=%02X n=%u", (unsigned long long)g_frame, (uint32_t)((const uint8_t*)b - base), op, cnt);
            for (uint32_t k = 0; k < cnt && k < 8; k++) s += S(" %08X", b[k]);
            fprintf(stderr, "%s\n", s.c_str());
            static const uint32_t* last = nullptr;                          // the raw words since the previous packet, which carry the type 0 register writes
            if (last && b > last && b - last < 512) {
                s = "[pm4raw]";
                for (const uint32_t* w = last; w < b; w++) s += S(" %08X", *w);
                fprintf(stderr, "%s\n", s.c_str());
            }
            last = b + cnt;
        }
    }
    auto load_shader = [&](uint32_t type, const uint32_t* data, uint32_t n, bool fromMem, uint32_t addr) {
        auto& code = type == 0 ? g_vsCode : g_psCode;
        static std::vector<uint32_t> incoming;
        incoming.resize(n);
        if (fromMem) for (uint32_t i = 0; i < n; i++) incoming[i] = _byteswap_ulong(*(const uint32_t*)phys(addr + i * 4));
        else memcpy(incoming.data(), data, n * 4);
        if (incoming.size() == code.size() && !memcmp(incoming.data(), code.data(), n * 4)) return;   // the same shader again: keep its generation
        code.swap(incoming);
        (type == 0 ? g_vsGen : g_psGen)++;
    };
    auto const_base = [](uint32_t ot, uint32_t& idx) {
        idx = ot & 0x7FF;
        switch ((ot >> 16) & 0xFF) {
          case 0: idx += R_CONST; return true;
          case 1: idx += R_FETCH; return true;
          case 2: idx += R_BOOL; return true;
          case 3: idx += R_LOOP; return true;
          case 4: idx += 0x2000; return true;
        }
        return false;
    };
    switch (op) {
      case 0x21: {                                                          // REG_RMW
        uint32_t ri = b[0], reg = ri & 0x1FFF, v = g_regs[reg];
        if (ri >> 31) v &= g_regs[b[1] & 0x1FFF]; else v &= b[1];
        if ((ri >> 30) & 1) v |= g_regs[b[2] & 0x1FFF]; else v |= b[2];
        gpu_reg(reg, v);
        break; }
      case 0x22: {                                                          // DRAW_INDX
        if (cnt < 2) break;
        uint32_t init = b[1], sel = (init >> 6) & 3;
        gpu_reg(0x21FC, init);
        if (sel == 0 && cnt >= 4) issue_draw(init, nullptr, b[2], b[3]);
        else if (sel == 2) issue_draw(init, nullptr, 0, 0);
        else issue_draw(init, cnt > 2 ? b + 2 : nullptr, 0, 0);
        break; }
      case 0x36: {                                                          // DRAW_INDX_2
        uint32_t init = b[0], sel = (init >> 6) & 3;
        gpu_reg(0x21FC, init);
        if (sel == 2) issue_draw(init, nullptr, 0, 0);
        else if (sel == 1) issue_draw(init, b + 1, 0, 0);
        else if (cnt >= 3) issue_draw(init, nullptr, b[1], b[2]);
        break; }
      case 0x2D: {                                                          // SET_CONSTANT
        uint32_t idx; if (!const_base(b[0], idx)) break;
        const_watch("SET_CONSTANT", idx, cnt - 1, b + 1, 0);
        for (uint32_t k = 1; k < cnt; k++) gpu_reg(idx++, b[k]);
        break; }
      case 0x2F: {                                                          // LOAD_ALU_CONSTANT
        uint32_t idx; if (!const_base(b[1], idx)) break;
        uint32_t addr = b[0] & 0x3FFFFFFC, n = b[2] & 0xFFF;
        if (g_trace > 0) { std::vector<uint32_t> v(n); for (uint32_t k = 0; k < n; k++) v[k] = _byteswap_ulong(*(const uint32_t*)phys(addr + k * 4)); const_watch("LOAD_ALU_CONSTANT", idx, n, v.data(), addr); }
        for (uint32_t k = 0; k < n; k++) gpu_reg(idx++, _byteswap_ulong(*(const uint32_t*)phys(addr + k * 4)));
        break; }
      case 0x55: case 0x56: {                                               // SET_CONSTANT2, SET_SHADER_CONSTANTS
        uint32_t idx = b[0] & 0xFFFF;
        const_watch(op == 0x55 ? "SET_CONSTANT2" : "SET_SHADER_CONSTANTS", idx, cnt - 1, b + 1, 0);
        for (uint32_t k = 1; k < cnt; k++) gpu_reg(idx++, b[k]);
        break; }
      case 0x27: load_shader(b[0] & 3, nullptr, b[1] & 0xFFFF, true, b[0] & ~3u); break;   // IM_LOAD
      case 0x2B: load_shader(b[0] & 3, b + 2, b[1] & 0xFFFF, false, 0); break;             // IM_LOAD_IMMEDIATE
      case 0x5B: {                                                          // EVENT_WRITE_ZPD: occlusion query report
        // The game draws glows and light halos only if its query reports visible samples. Answer as Xenia does by default: a
        // finished query (marked 0xFFFFFEED) gets a fixed count, so everything counts as visible. Fields are stored little-endian.
        static const bool kNoFake = getenv("ASURA_NOOCCLUSIONFAKE") != nullptr;
        uint32_t addr = g_regs[0x2325];                                     // RB_SAMPLE_COUNT_ADDR
        if (kNoFake || !addr) break;
        uint32_t* c = (uint32_t*)phys(addr & ~3u);                          // Total_A/B, ZFail_A/B, ZPass_A/B, StencilFail_A/B
        const uint32_t kDone = _byteswap_ulong(0xFFFFFEEDu);
        bool endZPass = c[4] == kDone && c[5] == kDone, endZFail = c[2] == kDone && c[3] == kDone;
        if (g_trace > 0) fprintf(stderr, "[zpd] f=%llu at %08X before: %08X %08X %08X %08X %08X %08X %08X %08X%s\n", (unsigned long long)g_frame, addr,
                                 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], (endZPass || endZFail) ? " end" : "");
        memset(c, 0, 32);

        // ASURA_OCCLUSION=1 (experiment): real counts. A report without the end markers starts a host occlusion query, the next
        // one with them ends it; the count (host samples over the render scale squared) is written at once (a GPU sync per query).
        static const bool kReal = getenv("ASURA_OCCLUSION") != nullptr;
        static ComPtr<ID3D11Query> open;
        if (kReal) {
            if (!(endZPass || endZFail)) {
                if (!open) { D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION, 0 }; g_dev->CreateQuery(&qd, &open); if (open) g_ctx->Begin(open.Get()); }
                break;
            }
            uint64_t n = 0;
            if (open) {
                g_ctx->End(open.Get());
                while (g_ctx->GetData(open.Get(), &n, sizeof n, 0) == S_FALSE) SwitchToThread();
                open.Reset();
            }
            n /= (uint64_t)(g_resScale * g_resScale);
            const uint32_t v = (uint32_t)std::min<uint64_t>(n, 0x7FFFFFFF);
            c[4] = v; c[0] = v;
            if (g_trace > 0) fprintf(stderr, "[zpd] f=%llu real count %u\n", (unsigned long long)g_frame, v);
            break;
        }
        if (endZPass || endZFail) { c[4] = 1000; c[0] = 1000; }
        static int nl = 0;
        if (nl++ < 4) fprintf(stderr, "[gfx] occlusion query report at %08X finished=%d\n", addr, (int)(endZPass || endZFail));
        break; }
      case 0x64: do_swap(b[1]); break;                                      // XE_SWAP
      default: break;
    }
}
