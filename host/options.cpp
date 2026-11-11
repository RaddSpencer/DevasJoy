// Graphics Options: a menu the host draws over the title screen (the game's own menus are Scaleform movies, out of reach for now; NOTES.md).
// Opened with the pad's Back (View) button or F1 while the title / main menu is up (kfile.cpp, title_screen_active). D-pad or left stick and the
// arrow keys pick and change; B or Escape closes. While it is open the game gets a neutral pad (xam.cpp, options_capturing).
// Settings live in asura.ini under the data root. An ASURA_* variable set by hand still wins, so test runs are unaffected.
//   resscale = 0 (auto: the smallest multiple of 1280x720 covering the display) or 1..4. Read once at launch: changing it offers a restart.
//   vsync    = 0 or 1. Applies at once.
//   gamma    = 50..150 (percent, 100 = as the game made it; lower is darker, for bright displays). Applies at once (present pass).
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
int gpu_render_scale();                           // xenos.cpp

namespace {
struct Settings { int resscale = 0; int vsync = 0; int gamma = 100; };
Settings g_saved;                                 // what asura.ini holds
bool g_envRes = false, g_envVsync = false;        // the variable came from the ini (so a restart may drop it), not from the user
std::string ini_path() { return bcr2_data_root() + "\\asura.ini"; }
void load(Settings& s) {
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, " resscale = %d", &v) == 1) s.resscale = v < 0 ? 0 : v > 4 ? 4 : v;
            else if (sscanf(line, " vsync = %d", &v) == 1) s.vsync = v ? 1 : 0;
            else if (sscanf(line, " gamma = %d", &v) == 1) s.gamma = v < 50 ? 50 : v > 150 ? 150 : v;
        }
        fclose(f);
    }
}
void save(const Settings& s) {
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "; Asura's Wrath recomp settings (written by the Graphics Options menu)\n");
        fprintf(f, "; resscale: 0 = auto, 1-4 = internal resolution as a multiple of 1280x720 (applies after a restart)\n");
        fprintf(f, "resscale = %d\n", s.resscale);
        fprintf(f, "; vsync: 0 = off, 1 = on\n");
        fprintf(f, "vsync = %d\n", s.vsync);
        fprintf(f, "; gamma: 50-150 percent, 100 = unchanged, lower = darker\n");
        fprintf(f, "gamma = %d\n", s.gamma);
        fclose(f);
    }
}

// ---- menu state ----
std::atomic<bool> g_open{false}, g_capture{false};
std::mutex g_lock;                                // menu state below, between the GPU thread (input, drawing) and nobody else; kept for safety
Settings g_cur;                                   // what the menu shows
int g_row = 0;
uint64_t g_version = 1;                           // bumped when the picture changes
enum Row { kRes, kVsync, kGamma, kRestart, kClose, kRows };

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
    return want != gpu_render_scale();
}
bool row_enabled(int r) { return r != kRestart || restart_needed(); }

void restart() {
    // Variables the ini put in the environment would pin the old values in the new process: drop them so it reads the ini again.
    if (g_envRes) _putenv_s("ASURA_RESSCALE", "");
    if (g_envVsync) _putenv_s("ASURA_VSYNC", "");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        fprintf(stderr, "[options] restarting for the new internal resolution\n");
        fflush(stderr);
        ExitProcess(0);
    }
    fprintf(stderr, "[options] restart failed (%lu)\n", GetLastError());
}

void change(int dir) {
    if (g_row == kRes) { g_cur.resscale = (g_cur.resscale + dir + 5) % 5; }
    else if (g_row == kVsync) { g_cur.vsync ^= 1; gpu_set_vsync(g_cur.vsync != 0); }
    else if (g_row == kGamma) { g_cur.gamma = std::clamp(g_cur.gamma + dir * 5, 50, 150); gpu_set_gamma(g_cur.gamma); }
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
void settings_apply_to_env() {
    load(g_saved);
    g_cur = g_saved;
    char v[16];
    if (!getenv("ASURA_RESSCALE") && g_saved.resscale > 0) { snprintf(v, sizeof v, "%d", g_saved.resscale); _putenv_s("ASURA_RESSCALE", v); g_envRes = true; }
    if (!getenv("ASURA_VSYNC")) { snprintf(v, sizeof v, "%d", g_saved.vsync); _putenv_s("ASURA_VSYNC", v); g_envVsync = true; }
}
int settings_button_set() { return 0; }

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
    }
    static uint32_t prev = 0;
    static int repeatDir = 0; static ULONGLONG repeatAt = 0;
    const uint32_t m = read_input(), pressed = m & ~prev;
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
    const int unit = h / 12;                                                // row height
    auto font = [&](int px, int weight) { return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI"); };
    HFONT fTitle = font(unit * 7 / 10, FW_SEMIBOLD), fRow = font(unit * 11 / 20, FW_NORMAL), fHint = font(unit * 2 / 5, FW_NORMAL);
    auto text = [&](HFONT f, COLORREF c, int x, int y, int ww, int hh, const std::wstring& s, UINT fmt) {
        SelectObject(dc, f); SetTextColor(dc, c); RECT r{ x, y, x + ww, y + hh }; DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    const int pad = w / 20;
    text(fTitle, RGB(255, 255, 255), pad, unit / 2, w - 2 * pad, unit, L"Graphics Options", DT_LEFT);
    struct Line { std::wstring label, value; bool enabled; };
    const int cur = g_cur.resscale == 0 ? auto_scale() : g_cur.resscale;
    Line lines[kRows] = {
        { L"Internal Resolution", L"◀  " + res_text(g_cur.resscale) + L"  ▶", true },
        { L"Vsync", std::wstring(L"◀  ") + (g_cur.vsync ? L"On" : L"Off") + L"  ▶", true },
        { L"Gamma", L"◀  " + std::to_wstring(g_cur.gamma) + L"%  ▶", true },
        { L"Restart to apply", restart_needed() ? L"now rendering " + res_text(gpu_render_scale()) : L"", restart_needed() },
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
    std::wstring note = restart_needed() ? L"Internal resolution changes when the game restarts." : L"Settings are saved to asura.ini.";
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
