// What the shared Xenos renderer (xenos.cpp, from BCR2) expects from its host, for Asura's Wrath.
#include "kernel.h"
#include "kbm.h"
#include "paths.h"
#include <windows.h>

// files: the renderer keeps screenshots, its shader cache and manifest under the data root
std::string bcr2_data_root() { return data_root(); }
bool bcr2_log_requested() {   // also asked before main() runs
    static const bool r = getenv("ASURA_LOG") != nullptr || strstr(GetCommandLineA(), "--log") != nullptr;
    return r;
}
const char* get_content_root() { static const std::string r = data_root() + "\\game\\disc"; return r.c_str(); }

// BCR2's settings menu: not in this port yet, so the renderer keeps its defaults (and any ASURA_* switches)
void settings_apply_to_env() {}
int settings_button_set() { return 0; }

// keyboard and mouse: only the renderer's own keys (F9 capture, F12 screenshot) for now
void kbm_note_wheel(int) {}
void kbm_note_message(void*, unsigned, unsigned long long, long long) {}
bool kbm_reserved_down(int vk) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// BCR2's hardware data-breakpoint helpers (BCR2_HWWATCH*): not ported
void hw_watch_set(uint32_t) {}
void hw_watch_reads(const uint32_t*, int) {}
