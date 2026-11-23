// Keeps controller remappers from switching away from our window.
//
// Steam's controller layouts send keyboard and mouse input to whatever program is in front unless it is a Steam game: its chord layout
// (controller_base\chord_xboxone.vdf) turns Start, held with the chord button, into Alt+Tab (as a toggle, so Alt stays held and the switcher stays
// open), and its desktop layout turns Start into Escape, Back into Tab and the right stick and triggers into the mouse. Other remappers do the
// same. Such input arrives as injected input (LLKHF_INJECTED / LLMHF_INJECTED), so low-level hooks can drop it while one of this process's windows
// is in front, without touching the real keyboard and mouse (Alt+Tab on the keyboard still works).
//
//   input_guard_install(blockMouse)   call on a thread that runs a message loop (low-level hooks are called through it)
//
// Dropped while we are in front: injected Alt, Tab, Escape and Windows keys; with blockMouse, all injected mouse input (the game reads only the pad).
// ASURA_ALLOWINJECTED=1 turns it off.
#pragma once
#include <windows.h>
#include <cstdlib>

namespace inputguard {

inline bool we_are_in_front() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

inline bool g_blockMouse = false;

inline LRESULT CALLBACK key_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = (const KBDLLHOOKSTRUCT*)lp;
        if ((k->flags & LLKHF_INJECTED) && we_are_in_front()) {
            switch (k->vkCode) {
            case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_TAB: case VK_ESCAPE: case VK_LWIN: case VK_RWIN:
                return 1;   // swallowed
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_blockMouse) {
        const auto* m = (const MSLLHOOKSTRUCT*)lp;
        if ((m->flags & LLMHF_INJECTED) && we_are_in_front()) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline void install(bool blockMouse) {
    if (getenv("ASURA_ALLOWINJECTED")) return;
    g_blockMouse = blockMouse;
    SetWindowsHookExW(WH_KEYBOARD_LL, key_hook, GetModuleHandleW(nullptr), 0);
    if (blockMouse) SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0);
}

}  // namespace inputguard

inline void input_guard_install(bool blockMouse) { inputguard::install(blockMouse); }
