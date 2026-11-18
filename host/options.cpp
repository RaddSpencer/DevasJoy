// Graphics Options: a menu the host draws over the title screen (the game's own menus are Scaleform movies, out of reach for now; NOTES.md).
// Opened with the pad's Back (View) button or F1 while the title / main menu is up (kfile.cpp, title_screen_active). D-pad or left stick and the
// arrow keys pick and change; B or Escape closes. While it is open the game gets a neutral pad (xam.cpp, options_capturing).
// Settings live in asura.ini under the data root. An ASURA_* variable set by hand still wins, so test runs are unaffected.
//   resscale = 0 (auto: the smallest multiple of 1280x720 covering the display) or 1..4. Read once at launch: changing it offers a restart.
//   vsync    = 0 off, 1 on (both apply at once), 2 on for FreeSync / G-Sync displays: tearing allowed so the display follows the game,
//              frames released on an even cadence (ASURA_VRR=1, xenos.cpp); needs a restart (the swap chain is made for it).
//   gamma    = 50..150 (percent, 100 = as the game made it; lower is darker, for bright displays). Applies at once (present pass).
//   showfps  = 0 or 1: frames per second and frame time in the top-left corner (ASURA_SHOWFPS=1 by hand).
//   aa        = 0 off, 1 FXAA, 2 FXAA high (a wider edge search), in the final pass (ASURA_AA by hand). Applies at once.
//   buttons   = 0 Xbox (the game's own icons), 2 PlayStation, 3 Switch: button prompts from assets\prompts\<set>\ (tools\make_prompts.ps1),
//               matched by texture hash (xenos.cpp mods_texture); applies to textures loaded after the change, so from the next episode on.
//   texfilter = 0 as the game sets each texture, 1 bilinear, 2 trilinear, 3-6 anisotropic 2x 4x 8x 16x, for the textures the game
//               already filters (xenos.cpp get_sampler; point-sampled data and depth textures are left alone) (ASURA_TEXFILTER by hand).
//   fps      = 24, 30 or 60 (ASURA_FPS by hand; ASURA_FPS60=1 still means 60). Other than 30 needs both of the game's caps changed: the engine config's MaxSmoothedFrameRate (kfile.cpp reads it as 60:
//              ASURA_FPS60=1) and presenting every second vblank (the vblank runs at 120 Hz: ASURA_VBLANKHZ=120). Read at launch: restart.
//              24: the config reads 24 and the vblank runs at 48 Hz. Shown one of two ways (pulldown):
//   pulldown = 0: presented as soon as ready with tearing allowed (ASURA_VRR=1), for G-Sync / FreeSync displays; without one the
//              picture tears. 1: 2:3 pulldown as on DVD players: frames held for 3 and 2 refreshes of a 60 Hz display in turn (vsync,
//              no tearing, film judder; ASURA_PULLDOWN=1; xenos.cpp works out the cadence for other refresh rates).
// Story Mode (uescript.cpp), each piece its own setting, applied at once; the Story Mode row turns both on or off together:
//   invincible = 0 or 1: the player takes no damage (ASURA_INVINCIBLE=1 by hand).
//   qteskip    = 0 or 1: QTEs succeed by themselves with no prompt (ASURA_QTESKIP=1 by hand). BURST prompts stay with the player.
#define NOMINMAX
#include "paths.h"
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

bool title_screen_active();                       // kfile.cpp
bool kbm_reserved_down(int vk);                   // xenos_glue.cpp
void gpu_set_vsync(bool on);                      // xenos.cpp
void gpu_set_gamma(int percent);                  // xenos.cpp
void gpu_set_aa(int mode);                        // xenos.cpp
void gpu_set_texfilter(int mode);                 // xenos.cpp
void gpu_set_button_set(int s);                   // xenos.cpp
int gpu_render_scale();                           // xenos.cpp
std::string gpu_fps_text();                       // xenos.cpp
void video_finish();                              // video.cpp
void story_set(bool invincible, bool qteskip);    // uescript.cpp
bool story_invincible();
bool story_qteskip();

namespace {
struct Settings { int resscale = 0; int vsync = 0; int gamma = 100; int showfps = 0; int fps = 30; int pulldown = 0; int invincible = 0; int qteskip = 0; int aa = 0; int texfilter = 0; int buttons = 0; };
Settings g_saved;                                 // what asura.ini holds
bool g_envRes = false, g_envVsync = false, g_envFps = false, g_envVblank = false, g_envVrr = false, g_envPulldown = false;   // the variable came from the ini (a restart drops it), not from the user
int g_runFps = 30, g_runPulldown = 0, g_runVrr = 0;            // the frame rate (and 24 fps display) this process started with
std::string ini_path() { return bcr2_data_root() + "\\asura.ini"; }
void load(Settings& s) {
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, " resscale = %d", &v) == 1) s.resscale = v < 0 ? 0 : v > 4 ? 4 : v;
            else if (sscanf(line, " vsync = %d", &v) == 1) s.vsync = v < 0 ? 0 : v > 2 ? 2 : v;
            else if (sscanf(line, " gamma = %d", &v) == 1) s.gamma = v < 50 ? 50 : v > 150 ? 150 : v;
            else if (sscanf(line, " showfps = %d", &v) == 1) s.showfps = v ? 1 : 0;
            else if (sscanf(line, " fps = %d", &v) == 1) s.fps = v >= 60 ? 60 : 30;   // 24 is no longer offered: an old ini's 24 runs at 30
            else if (sscanf(line, " pulldown = %d", &v) == 1) s.pulldown = v ? 1 : 0;
            else if (sscanf(line, " invincible = %d", &v) == 1) s.invincible = v ? 1 : 0;
            else if (sscanf(line, " qteskip = %d", &v) == 1) s.qteskip = v ? 1 : 0;
            else if (sscanf(line, " aa = %d", &v) == 1) s.aa = std::clamp(v, 0, 2);
            else if (sscanf(line, " texfilter = %d", &v) == 1) s.texfilter = std::clamp(v, 0, 6);
            else if (sscanf(line, " buttons = %d", &v) == 1) s.buttons = (v == 2 || v == 3) ? v : 0;
        }
        fclose(f);
    }
}
void save(const Settings& s) {
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "; Asura's Wrath recomp settings (written by the Graphics Options menu)\n");
        fprintf(f, "; resscale: 0 = auto, 1-4 = internal resolution as a multiple of 1280x720 (applies after a restart)\n");
        fprintf(f, "resscale = %d\n", s.resscale);
        fprintf(f, "; vsync: 0 = off, 1 = on, 2 = on for FreeSync / G-Sync displays (even frame pacing; applies after a restart)\n");
        fprintf(f, "vsync = %d\n", s.vsync);
        fprintf(f, "; gamma: 50-150 percent, 100 = unchanged, lower = darker\n");
        fprintf(f, "gamma = %d\n", s.gamma);
        fprintf(f, "; showfps: 1 = frames per second in the top-left corner\n");
        fprintf(f, "showfps = %d\n", s.showfps);
        fprintf(f, "; fps: 30 (as on the console) or 60 (applies after a restart)\n");
        fprintf(f, "fps = %d\n", s.fps);
        fprintf(f, "; pulldown (24 fps only): 0 = for G-Sync / FreeSync displays (tears without one), 1 = 2:3 pulldown on a 60 Hz display\n");
        fprintf(f, "pulldown = %d\n", s.pulldown);
        fprintf(f, "; Story Mode: invincible = 1, the player takes no damage; qteskip = 1, QTEs succeed by themselves (BURST stays with the player)\n");
        fprintf(f, "invincible = %d\n", s.invincible);
        fprintf(f, "qteskip = %d\n", s.qteskip);
        fprintf(f, "; aa: anti-aliasing, 0 = off, 1 = FXAA, 2 = FXAA high\n");
        fprintf(f, "aa = %d\n", s.aa);
        fprintf(f, "; texfilter: 0 = as the game sets it, 1 = bilinear, 2 = trilinear, 3-6 = anisotropic 2x, 4x, 8x, 16x\n");
        fprintf(f, "texfilter = %d\n", s.texfilter);
        fprintf(f, "; buttons: button prompts, 0 = Xbox (as the game draws them), 2 = PlayStation, 3 = Switch\n");
        fprintf(f, "buttons = %d\n", s.buttons);
        fclose(f);
    }
}

// ---- menu state ----
std::atomic<bool> g_open{false}, g_capture{false};
std::mutex g_lock;                                // menu state below, between the GPU thread (input, drawing) and nobody else; kept for safety
Settings g_cur;                                   // what the menu shows
int g_row = 0;
uint64_t g_version = 1;                           // bumped when the picture changes
enum Row { kRes, kRate, kVsync, kGamma, kAA, kTex, kButtons, kFps, kStory, kInvincible, kQteSkip, kRestart, kClose, kRows };
std::atomic<bool> g_showFps{false};

int auto_scale() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    int s = 1;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) s = (int)std::max((dm.dmPelsWidth + 1279) / 1280, (dm.dmPelsHeight + 719) / 720);
    return s < 1 ? 1 : s > 4 ? 4 : s;
}
std::wstring res_text(int s) {
    if (s == 0) { const int a = auto_scale(); return L"Auto (" + std::to_wstring(1280 * a) + L" x " + std::to_wstring(720 * a) + L")"; }
    return std::to_wstring(1280 * s) + L" x " + std::to_wstring(720 * s);
}
bool restart_needed() {
    const int want = g_cur.resscale == 0 ? auto_scale() : g_cur.resscale;
    const int wantVrr = g_cur.fps == 24 ? !g_cur.pulldown : g_cur.vsync == 2;
    return want != gpu_render_scale() || g_cur.fps != g_runFps || (g_cur.fps == 24 && g_cur.pulldown != g_runPulldown) || wantVrr != g_runVrr;
}
// The Vsync row holds the 24 fps modes too: 0 Off, 1 On, 2 24 fps for FreeSync / G-Sync displays, 3 24 fps with 2:3 pulldown.
// The 24 fps choices set fps = 24 (and pulldown); Frame Rate then shows 24 and can't be changed until Vsync is set back to Off or On.
int vsync_choice() { return g_cur.fps == 24 ? (g_cur.pulldown ? 4 : 3) : g_cur.vsync; }   // 0 Off, 1 On, 2 On (VRR), 3 24 fps VRR, 4 24 fps pulldown
int g_lastRate = 30;                              // the 30 / 60 setting to return to when Vsync leaves the 24 fps choices
bool row_enabled(int r) { return (r != kRestart || restart_needed()) && (r != kRate || g_cur.fps != 24); }

void restart() {
    // Variables the ini put in the environment would pin the old values in the new process: drop them so it reads the ini again.
    if (g_envRes) _putenv_s("ASURA_RESSCALE", "");
    if (g_envVsync) _putenv_s("ASURA_VSYNC", "");
    if (g_envFps) _putenv_s("ASURA_FPS", "");
    if (g_envVrr) _putenv_s("ASURA_VRR", "");
    if (g_envPulldown) _putenv_s("ASURA_PULLDOWN", "");
    if (g_envVblank) _putenv_s("ASURA_VBLANKHZ", "");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        fprintf(stderr, "[options] restarting for the new internal resolution or frame rate\n");
        fflush(stderr);
        video_finish();                                                     // video.cpp: keep a recording playable
        ExitProcess(0);
    }
    fprintf(stderr, "[options] restart failed (%lu)\n", GetLastError());
}

void change(int dir) {
    if (g_row == kRes) { g_cur.resscale = (g_cur.resscale + dir + 5) % 5; }
    else if (g_row == kRate) { if (g_cur.fps == 24) return; g_cur.fps = g_lastRate = g_cur.fps == 60 ? 30 : 60; }
    else if (g_row == kVsync) {                                             // Off, On, 24 fps (FreeSync / G-Sync), 24 fps (2:3 pulldown)
        const int i = (std::min(vsync_choice(), 2) + dir + 3) % 3;          // the 24 fps choices were removed (2026-11-14, the user's call)
        if (i < 3) { if (g_cur.fps == 24) g_cur.fps = g_lastRate; g_cur.vsync = i; if (!g_runVrr) gpu_set_vsync(i == 1); }
        else { if (g_cur.fps != 24) g_lastRate = g_cur.fps; g_cur.fps = 24; g_cur.pulldown = i == 4; }
    }
    else if (g_row == kGamma) { g_cur.gamma = std::clamp(g_cur.gamma + dir * 5, 50, 150); gpu_set_gamma(g_cur.gamma); }
    else if (g_row == kAA) { g_cur.aa = (g_cur.aa + dir + 3) % 3; gpu_set_aa(g_cur.aa); }
    else if (g_row == kTex) { g_cur.texfilter = (g_cur.texfilter + dir + 7) % 7; gpu_set_texfilter(g_cur.texfilter); }
    else if (g_row == kButtons) { static const int order[3] = { 0, 2, 3 }; int i = g_cur.buttons == 2 ? 1 : g_cur.buttons == 3 ? 2 : 0; g_cur.buttons = order[(i + dir + 3) % 3]; gpu_set_button_set(g_cur.buttons); }
    else if (g_row == kFps) { g_cur.showfps ^= 1; g_showFps = g_cur.showfps != 0; }
    else if (g_row == kStory) { const int on = (g_cur.invincible && g_cur.qteskip) ? 0 : 1; g_cur.invincible = g_cur.qteskip = on; story_set(on, on); }
    else if (g_row == kInvincible) { g_cur.invincible ^= 1; story_set(g_cur.invincible, g_cur.qteskip); }
    else if (g_row == kQteSkip) { g_cur.qteskip ^= 1; story_set(g_cur.invincible, g_cur.qteskip); }
    else return;
    g_saved = g_cur; save(g_saved);
    g_version++;
}
void move(int dir) {
    for (int i = 0; i < kRows; i++) { g_row = (g_row + dir + kRows) % kRows; if (row_enabled(g_row)) break; }
    g_version++;
}

// Buttons as a bit set, from any connected pad and the arrow / Enter / Escape keys (window in front).
enum : uint32_t { bUp = 1, bDown = 2, bLeft = 4, bRight = 8, bOk = 16, bCancel = 32, bOpen = 64 };
uint32_t read_input() {
    uint32_t m = 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE s;
        if (XInputGetState(i, &s) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& g = s.Gamepad;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) m |= bUp;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) m |= bDown;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) m |= bLeft;
        if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) m |= bRight;
        if (g.wButtons & XINPUT_GAMEPAD_A) m |= bOk;
        if (g.wButtons & XINPUT_GAMEPAD_B) m |= bCancel;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) m |= bOpen;
    }
    if (kbm_reserved_down(VK_UP)) m |= bUp;
    if (kbm_reserved_down(VK_DOWN)) m |= bDown;
    if (kbm_reserved_down(VK_LEFT)) m |= bLeft;
    if (kbm_reserved_down(VK_RIGHT)) m |= bRight;
    if (kbm_reserved_down(VK_RETURN)) m |= bOk;
    if (kbm_reserved_down(VK_ESCAPE)) m |= bCancel;
    if (kbm_reserved_down(VK_F1)) m |= bOpen;
    return m;
}
}  // namespace

// Before the renderer reads its switches (xenos.cpp calls this first): the ini's values, unless the variable is already set.
// The frame rate asked for: ASURA_FPS=24|30|60, or ASURA_FPS60=1 (60). kfile.cpp reads the engine config's 30 as this.
int settings_target_fps() {
    if (const char* e = getenv("ASURA_FPS")) { const int v = atoi(e); return v >= 60 ? 60 : v > 0 && v <= 24 ? 24 : 30; }
    if (const char* e = getenv("ASURA_FPS60")) if (atoi(e)) return 60;
    return 30;
}
void settings_apply_to_env() {
    load(g_saved);
    g_cur = g_saved;
    char v[16];
    if (!getenv("ASURA_RESSCALE") && g_saved.resscale > 0) { snprintf(v, sizeof v, "%d", g_saved.resscale); _putenv_s("ASURA_RESSCALE", v); g_envRes = true; }
    if (!getenv("ASURA_VSYNC")) { snprintf(v, sizeof v, "%d", g_saved.vsync == 1 ? 1 : 0); _putenv_s("ASURA_VSYNC", v); g_envVsync = true; }
    if (g_saved.vsync == 2 && g_saved.fps != 24 && !getenv("ASURA_VRR")) { _putenv_s("ASURA_VRR", "1"); g_envVrr = true; }
    if (!getenv("ASURA_FPS") && !getenv("ASURA_FPS60") && g_saved.fps != 30) { _putenv_s("ASURA_FPS", g_saved.fps == 60 ? "60" : "24"); g_envFps = true; }
    const int fps = settings_target_fps();
    if (fps != 30 && !getenv("ASURA_VBLANKHZ")) { _putenv_s("ASURA_VBLANKHZ", fps == 60 ? "120" : "48"); g_envVblank = true; }   // the game presents every second vblank
    if (fps == 24 && !getenv("ASURA_PULLDOWN") && !getenv("ASURA_VRR")) {
        if (g_saved.pulldown) { _putenv_s("ASURA_PULLDOWN", "1"); g_envPulldown = true; }
        else { _putenv_s("ASURA_VRR", "1"); g_envVrr = true; }
    }
    g_runFps = fps;
    g_runPulldown = getenv("ASURA_PULLDOWN") && atoi(getenv("ASURA_PULLDOWN")) != 0;
    g_runVrr = getenv("ASURA_VRR") != nullptr;
    g_cur.fps = g_saved.fps = g_runFps;                                     // the menu shows the frame rate in use, however it was set
    if (!g_envVrr && g_runVrr && g_runFps != 24) g_cur.vsync = g_saved.vsync = 2;   // ASURA_VRR=1 set by hand: show the mode in use
    else if (!g_envVsync && !g_runVrr) { const char* e = getenv("ASURA_VSYNC"); if (e) g_cur.vsync = g_saved.vsync = atoi(e) ? 1 : 0; }
    if (fps == 24) g_cur.pulldown = g_saved.pulldown = g_runPulldown;
    else g_lastRate = fps;
}
int settings_button_set() {                                                 // read by xenos.cpp at the first texture (ASURA_BUTTONS=0|2|3 by hand wins)
    if (const char* e = getenv("ASURA_BUTTONS")) { const int v = atoi(e); return (v == 2 || v == 3) ? v : 0; }
    return g_saved.buttons;
}

bool options_open() { return g_open.load(); }
bool options_capturing() { return g_capture.load(); }

// Once per frame from the renderer's swap.
void options_frame() {
    std::lock_guard<std::mutex> lk(g_lock);
    static bool applied = false;                                            // the saved gamma, once the renderer is running (ASURA_GAMMA wins)
    if (!applied) {
        applied = true;
        if (const char* e = getenv("ASURA_GAMMA")) g_saved.gamma = std::clamp(atoi(e), 50, 150);   // shown and kept as the value in use
        g_cur.gamma = g_saved.gamma;
        gpu_set_gamma(g_saved.gamma);
        if (const char* e = getenv("ASURA_SHOWFPS")) g_saved.showfps = atoi(e) ? 1 : 0;
        g_cur.showfps = g_saved.showfps;
        g_showFps = g_saved.showfps != 0;
        if (const char* e = getenv("ASURA_AA")) g_saved.aa = std::clamp(atoi(e), 0, 2);   // set by hand: shown and kept as the value in use
        if (const char* e = getenv("ASURA_TEXFILTER")) g_saved.texfilter = std::clamp(atoi(e), 0, 6);
        g_saved.buttons = settings_button_set();
        g_cur.aa = g_saved.aa; g_cur.texfilter = g_saved.texfilter; g_cur.buttons = g_saved.buttons;
        gpu_set_aa(g_saved.aa); gpu_set_texfilter(g_saved.texfilter);
        story_set(g_saved.invincible, g_saved.qteskip);                     // ASURA_INVINCIBLE / ASURA_QTESKIP set by hand win (story_set leaves them)
        g_cur.invincible = g_saved.invincible = story_invincible() ? 1 : 0;   // shown as the values in use
        g_cur.qteskip = g_saved.qteskip = story_qteskip() ? 1 : 0;
    }
    static uint32_t prev = 0;
    static int repeatDir = 0; static ULONGLONG repeatAt = 0;
    static const bool kTestNoInput = getenv("ASURA_OPTIONSTEST") != nullptr;  // test runs: a pad on the desk must not close the menu
    const uint32_t m = kTestNoInput ? 0u : read_input(), pressed = m & ~prev;
    prev = m;
    if (!g_open) {
        static const bool kTest = getenv("ASURA_OPTIONSTEST") != nullptr;      // test runs: open by itself the first time the title is up
        static bool tested = false;
        if (((pressed & bOpen) || (kTest && !tested)) && title_screen_active()) {
            tested = true;
            g_open = true; g_capture = true; g_row = kRes; g_cur = g_saved; g_version++;
            fprintf(stderr, "[options] Graphics Options opened\n");
        } else if (g_capture && !(m & (bOk | bCancel | bOpen | bUp | bDown | bLeft | bRight))) g_capture = false;   // keep the closing press from the game
        return;
    }
    if (!title_screen_active() || (pressed & (bCancel | bOpen))) { g_open = false; g_version++; fprintf(stderr, "[options] Graphics Options closed\n"); return; }
    if (pressed & bUp) move(-1);
    if (pressed & bDown) move(1);
    // left/right repeat while held, for stepping through resolutions
    const int dir = (m & bLeft) ? -1 : (m & bRight) ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (dir && (pressed & (bLeft | bRight))) { change(dir); repeatDir = dir; repeatAt = now + 400; }
    else if (dir && dir == repeatDir && now >= repeatAt) { change(dir); repeatAt = now + 150; }
    else if (!dir) repeatDir = 0;
    if (pressed & bOk) {
        if (g_row == kRestart && restart_needed()) restart();
        else if (g_row == kClose) { g_open = false; g_version++; }
        else change(1);
    }
    if (!row_enabled(g_row)) move(1);
}

// The menu's picture, premultiplied RGBA at w x h, redrawn when the menu or the size changed. Returns false when there is nothing to show.
bool options_picture(int w, int h, std::vector<uint8_t>& rgba, uint64_t& version) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_open || w <= 0 || h <= 0) return false;
    static int lastW = 0, lastH = 0;
    if (version == g_version && w == lastW && h == lastH) return true;
    version = g_version; lastW = w; lastH = h;

    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    const int unit = h / 18;                                                // row height
    auto font = [&](int px, int weight) { return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI"); };
    HFONT fTitle = font(unit * 7 / 10, FW_SEMIBOLD), fRow = font(unit * 11 / 20, FW_NORMAL), fHint = font(unit * 2 / 5, FW_NORMAL);
    auto text = [&](HFONT f, COLORREF c, int x, int y, int ww, int hh, const std::wstring& s, UINT fmt) {
        SelectObject(dc, f); SetTextColor(dc, c); RECT r{ x, y, x + ww, y + hh }; DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    const int pad = w / 20;
    text(fTitle, RGB(255, 255, 255), pad, unit / 2, w - 2 * pad, unit, L"Options", DT_LEFT);
    struct Line { std::wstring label, value; bool enabled; };
    const int cur = g_cur.resscale == 0 ? auto_scale() : g_cur.resscale;
    Line lines[kRows] = {
        { L"Internal Resolution", L"◀  " + res_text(g_cur.resscale) + L"  ▶", true },
        { L"Frame Rate", g_cur.fps == 24 ? std::wstring(L"24 fps (set by Vsync)") : L"◀  " + std::to_wstring(g_cur.fps) + L" fps  ▶", g_cur.fps != 24 },
        { L"Vsync", std::wstring(L"◀  ") + (vsync_choice() == 0 ? L"Off" : vsync_choice() == 1 ? L"On" : vsync_choice() == 2 ? L"On (FreeSync / G-Sync)" : vsync_choice() == 3 ? L"24 fps (FreeSync / G-Sync recommended)" : L"24 fps (2:3 pulldown)") + L"  ▶", true },
        { L"Gamma", L"◀  " + std::to_wstring(g_cur.gamma) + L"%  ▶", true },
        { L"Anti-aliasing", std::wstring(L"◀  ") + (g_cur.aa == 0 ? L"Off" : g_cur.aa == 1 ? L"FXAA" : L"FXAA (high)") + L"  ▶", true },
        { L"Texture Filtering", L"◀  " + std::wstring(g_cur.texfilter == 0 ? L"As the game sets it" : g_cur.texfilter == 1 ? L"Bilinear" : g_cur.texfilter == 2 ? L"Trilinear"
                                 : L"Anisotropic " + std::to_wstring(1 << (g_cur.texfilter - 2)) + L"x") + L"  ▶", true },
        { L"Button Labels", std::wstring(L"◀  ") + (g_cur.buttons == 2 ? L"PlayStation" : g_cur.buttons == 3 ? L"Switch" : L"Xbox") + L"  ▶", true },
        { L"Show FPS", std::wstring(L"◀  ") + (g_cur.showfps ? L"On" : L"Off") + L"  ▶", true },
        { L"Story Mode", std::wstring(L"◀  ") + (g_cur.invincible && g_cur.qteskip ? L"On" : g_cur.invincible || g_cur.qteskip ? L"Custom" : L"Off") + L"  ▶", true },
        { L"    Invincibility", std::wstring(L"◀  ") + (g_cur.invincible ? L"On" : L"Off") + L"  ▶", true },
        { L"    Automatic QTEs", std::wstring(L"◀  ") + (g_cur.qteskip ? L"On" : L"Off") + L"  ▶", true },
        { L"Restart to apply", restart_needed() ? L"now " + res_text(gpu_render_scale()) + L", " + std::to_wstring(g_runFps) + L" fps" + (g_runFps == 24 ? (g_runPulldown ? L" pulldown" : L" VRR") : L"") : L"", restart_needed() },
        { L"Close", L"", true },
    };
    (void)cur;
    int y = unit * 2;
    for (int i = 0; i < kRows; i++, y += unit) {
        const bool sel = i == g_row;
        if (sel) {                                                          // highlight bar: drawn into the alpha later, marked here in blue
            RECT r{ pad / 2, y + unit / 10, w - pad / 2, y + unit - unit / 10 };
            HBRUSH b = CreateSolidBrush(RGB(0, 0, 90)); FillRect(dc, &r, b); DeleteObject(b);
        }
        const COLORREF c = !lines[i].enabled ? RGB(110, 110, 110) : sel ? RGB(255, 200, 80) : RGB(230, 230, 230);
        text(fRow, c, pad, y, w / 2, unit, lines[i].label, DT_LEFT);
        text(fRow, lines[i].enabled ? c : RGB(110, 110, 110), w / 2 - pad, y, w / 2, unit, lines[i].value, DT_RIGHT);
    }
    std::wstring note = g_row == kButtons ? L"Button labels change from the next episode or menu screen that loads." : restart_needed() ? L"Internal resolution and frame rate change when the game restarts." : L"Settings are saved to asura.ini.";
    if (g_cur.fps == 24 && (g_row == kVsync || !g_cur.pulldown))
        note = g_cur.pulldown ? L"24 fps shown 3:2 on a 60 Hz display, as DVD players do: no tearing, slight film judder."
                              : L"Warning: 24 fps needs a G-Sync or FreeSync display. Without one the picture will tear.";
    text(fHint, RGB(180, 180, 180), pad, h - unit * 2, w - 2 * pad, unit, note, DT_LEFT);
    text(fHint, RGB(180, 180, 180), pad, h - unit * 5 / 4, w - 2 * pad, unit, L"Up/Down: choose    Left/Right: change    A: select    B / Back: close", DT_LEFT);
    GdiFlush();

    // GDI leaves alpha alone: rebuild it. The panel is black at 85%; text and the highlight bar add their own colour (premultiplied).
    rgba.resize((size_t)w * h * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
        const uint8_t cov = std::max(r, std::max(g, b));
        rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = (uint8_t)std::max<int>(217, cov);
    }
    SelectObject(dc, oldBmp);
    DeleteObject(fTitle); DeleteObject(fRow); DeleteObject(fHint);
    DeleteObject(bmp); DeleteDC(dc);
    return true;
}

// The FPS counter's picture (Show FPS): the renderer's present rate and frame time, white on a dark box, redrawn when the text changes (twice a second).
bool fps_picture(int w, int h, std::vector<uint8_t>& rgba, uint64_t& version) {
    if (!g_showFps.load() || w <= 0 || h <= 0) return false;
    static std::string lastText; static int lastW = 0, lastH = 0; static uint64_t ver = 1;
    const std::string t = gpu_fps_text();
    if (t == lastText && w == lastW && h == lastH && version == ver) return true;
    if (t != lastText || w != lastW || h != lastH) ver++;
    lastText = t; lastW = w; lastH = h; version = ver;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontW(-(h * 3 / 4), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, f);
    SetTextColor(dc, RGB(255, 255, 255));
    const std::wstring ws(t.begin(), t.end());
    RECT r{ h / 4, 0, w, h }; DrawTextW(dc, ws.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    GdiFlush();
    rgba.resize((size_t)w * h * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) {                            // box black at 60%, text premultiplied over it
        const uint8_t b = src[i * 4], g = src[i * 4 + 1], r2 = src[i * 4 + 2];
        rgba[i * 4] = r2; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = (uint8_t)std::max<int>(153, std::max(r2, std::max(g, b)));
    }
    SelectObject(dc, oldFont); DeleteObject(f);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    return true;
}
