// xam: input, the local profile, content (saves and DLC), enumerators, notifications, system UI, messages, loader, tasks, net stubs.
// Saves live in <root>\saves\<file name> with a <file name>.xcontent beside each (the XCONTENT_DATA the game created it with).
// DLC is game\dlc\<package> (read only), named by game\dlc\<package>.name.
#include "kernel.h"
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "xinput.lib")

static constexpr uint32_t kTitleId = 0x43430817;
static constexpr uint64_t kXuid = 0xE000000000000001ull;
static constexpr uint32_t kDevice = 1;                 // the one storage device: "Hard Drive"
static constexpr uint32_t kContentDataSize = 0x134;    // XCONTENT_DATA {u32 device; u32 type; wchar name[128]; char file[42]; pad}

void complete_overlapped(uint32_t ov, uint32_t status, uint32_t length) {
    // XOVERLAPPED: +0 InternalLow (result), +4 InternalHigh (length), +8 InternalContext, +0xC event, +0x10 completion routine, +0x18 extended error
    wr32(ov + 0x04, length);
    wr32(ov + 0x18, status);
    wr32(ov + 0x00, status);
    if (uint32_t ev = rd32(ov + 0x0C)) event_set_by_handle(ev);
    if (uint32_t r = rd32(ov + 0x10)) LOG("[xam] overlapped %08X completion routine %08X not called\n", ov, r);
}
static uint32_t finish(uint32_t ov, uint32_t status, uint32_t length = 0) {
    if (ov) { complete_overlapped(ov, status, length); return ERROR_IO_PENDING; }
    return status;
}

// ======================================================================== input
static std::atomic<int> g_pad{-1};
static bool pad_read(XINPUT_STATE& s) {
    int p = g_pad.load();
    if (p >= 0) { if (XInputGetState((DWORD)p, &s) == ERROR_SUCCESS) return true; g_pad = -1; LOG("[input] controller %d disconnected\n", p); }
    static std::atomic<ULONGLONG> nextScan{0};
    ULONGLONG now = GetTickCount64();
    if (now < nextScan.load()) return false;
    nextScan = now + 2000;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) if (XInputGetState(i, &s) == ERROR_SUCCESS) { g_pad = (int)i; LOG("[input] controller %lu is user 0\n", i); return true; }
    return false;
}
// ASURA_AUTOINPUT="<once>|<loop>": one step per ASURA_AUTOINPUTMS (500) from 10 s after the first poll.
// S Start, B Back, A, b (B button), X, Y, U/D/L/R d-pad, u/d/l/r left stick, T/Z triggers, '.' nothing.
static bool auto_input(uint32_t st) {
    static const char* env = getenv("ASURA_AUTOINPUT");
    if (!env) return false;
    static const std::string pat = env;
    static const size_t bar = pat.find('|');
    static const std::string once = bar == std::string::npos ? pat : pat.substr(0, bar), loop = bar == std::string::npos ? "" : pat.substr(bar + 1);
    static const ULONGLONG t0 = GetTickCount64();
    static const uint32_t step = getenv("ASURA_AUTOINPUTMS") ? (uint32_t)atoi(getenv("ASURA_AUTOINPUTMS")) : 500;
    ULONGLONG el = GetTickCount64() - t0;
    uint16_t b = 0; int16_t lx = 0, ly = 0; uint8_t lt = 0, rt = 0; uint32_t cyc = 0;
    if (el >= 10000) {
        cyc = (uint32_t)((el - 10000) / step);
        char c = cyc < once.size() ? once[cyc] : (loop.empty() ? '.' : loop[(cyc - once.size()) % loop.size()]);
        switch (c) {
          case 'S': b = 0x0010; break; case 'B': b = 0x0020; break; case 'A': b = 0x1000; break; case 'b': b = 0x2000; break;
          case 'X': b = 0x4000; break; case 'Y': b = 0x8000; break; case 'U': b = 0x0001; break; case 'D': b = 0x0002; break;
          case 'L': b = 0x0004; break; case 'R': b = 0x0008; break; case 'u': ly = 32000; break; case 'd': ly = -32000; break;
          case 'l': lx = -32000; break; case 'r': lx = 32000; break; case 'T': rt = 255; break; case 'Z': lt = 255; break;
        }
    }
    wr32(st, cyc + 1); wr16(st + 4, b); g_base[st + 6] = lt; g_base[st + 7] = rt; wr16(st + 8, (uint16_t)lx); wr16(st + 10, (uint16_t)ly);
    return true;
}
// XamInputGetState(user, flags, XINPUT_STATE*): {u32 packet; u16 buttons; u8 lt, rt; s16 lx, ly, rx, ry}
static void XamInputGetState_(PPCContext& ctx, uint8_t*) {
    uint32_t user = ctx.r3.u32, st = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (!st) { ctx.r3.u64 = 0; return; }
    memset(g_base + st, 0, 16);
    if (!auto_input(st)) {
        XINPUT_STATE xs;
        if (pad_read(xs)) {
            const XINPUT_GAMEPAD& g = xs.Gamepad;
            wr32(st, xs.dwPacketNumber); wr16(st + 4, g.wButtons); g_base[st + 6] = g.bLeftTrigger; g_base[st + 7] = g.bRightTrigger;
            wr16(st + 8, (uint16_t)g.sThumbLX); wr16(st + 10, (uint16_t)g.sThumbLY); wr16(st + 12, (uint16_t)g.sThumbRX); wr16(st + 14, (uint16_t)g.sThumbRY);
        }
    }
    ctx.r3.u64 = 0;
}
static void XamInputSetState_(PPCContext& ctx, uint8_t*) {
    int p = g_pad.load();
    if (p >= 0 && ctx.r5.u32) { XINPUT_VIBRATION v{ rd16(ctx.r5.u32), rd16(ctx.r5.u32 + 2) }; XInputSetState((DWORD)p, &v); }
    ctx.r3.u64 = (ctx.r3.u32 == 0 || ctx.r3.u32 == 0xFF) ? 0 : ERROR_DEVICE_NOT_CONNECTED;
}
static void XamInputGetCapabilities_(PPCContext& ctx, uint8_t*) {
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (user != 0 && user != 0xFF) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    if (caps) { memset(g_base + caps, 0, 20); g_base[caps] = 1; g_base[caps + 1] = 1; wr16(caps + 2, 0); wr16(caps + 4, 0xF3FF); g_base[caps + 6] = 0xFF; g_base[caps + 7] = 0xFF; }
    ctx.r3.u64 = 0;
}

// ======================================================================== profile
static void XamUserGetSigninState_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = (ctx.r3.u32 == 0) ? 1 : 0; }   // signed in locally
static void XamUserGetXUID_(PPCContext& ctx, uint8_t*) {
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = 0x534; return; }   // ERROR_NO_SUCH_USER
    if (ctx.r5.u32) wr64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = 0;
}
static void XamUserGetName_(PPCContext& ctx, uint8_t*) {
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = 0x534; return; }
    const char* n = "Player";
    if (ctx.r4.u32 && ctx.r5.u32) { uint32_t k = std::min<uint32_t>((uint32_t)strlen(n), ctx.r5.u32 - 1); memcpy(g_base + ctx.r4.u32, n, k); g_base[ctx.r4.u32 + k] = 0; }
    ctx.r3.u64 = 0;
}
// XamUserGetSigninInfo(user, flags, info*): {u64 xuid; u32 unk; u32 state; u32 unk; u32 unk; char name[16]}
static void XamUserGetSigninInfo_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r5.u32;
    if (!p) { ctx.r3.u64 = 87; return; }
    memset(g_base + p, 0, 0x28);
    if (ctx.r3.u32 == 0) { wr64(p, kXuid); wr32(p + 0xC, 1); strcpy((char*)gp(p + 0x18), "Player"); ctx.r3.u64 = 0; }
    else ctx.r3.u64 = 0x534;
}
static void XamUserCheckPrivilege_(PPCContext& ctx, uint8_t*) { if (ctx.r5.u32) wr32(ctx.r5.u32, 1); ctx.r3.u64 = 0; }
static void XamUserAreUsersFriends_(PPCContext& ctx, uint8_t*) { if (ctx.r6.u32) wr32(ctx.r6.u32, 0); ctx.r3.u64 = finish(ctx.r7.u32, 0); }
static void XamUserGetMembershipTierFromXUID_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 6; }   // gold
static void XamUserGetOnlineCountryFromXUID_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 103; }  // US
// XamUserReadProfileSettings(title, user, xuid_count, xuids*, setting_count, setting_ids*, buffer_size*, buffer*, overlapped*)
// Answers every setting as "not set" so the game uses its defaults. Result: {u32 count; u32 settings*} then 0x28-byte entries
// {u32 source; u32 pad; u32 user; u32 pad; u32 id; u32 pad; X_USER_DATA {u8 type; pad[7]; u64 value}}.
static void XamUserReadProfileSettings_(PPCContext& ctx, uint8_t*) {
    uint32_t count = ctx.r7.u32, ids = ctx.r8.u32, sizeP = ctx.r9.u32, buf = ctx.r10.u32;
    uint32_t ov = rd32(ctx.r1.u32 + 0x54);
    uint32_t xuids = ctx.r5.u32 ? ctx.r5.u32 : 1;
    uint32_t need = 8 + count * xuids * 0x28;
    if (!buf || rd32(sizeP) < need) { wr32(sizeP, need); ctx.r3.u64 = 122; return; }   // ERROR_INSUFFICIENT_BUFFER
    memset(g_base + buf, 0, need);
    wr32(buf, count * xuids); wr32(buf + 4, buf + 8);
    for (uint32_t u = 0; u < xuids; u++) for (uint32_t i = 0; i < count; i++) {
        uint32_t e = buf + 8 + (u * count + i) * 0x28, id = rd32(ids + i * 4);
        wr32(e, 0); wr32(e + 8, ctx.r4.u32); wr32(e + 0x10, id); g_base[e + 0x18] = (uint8_t)(id >> 28);
    }
    LOG("[xam] XamUserReadProfileSettings %u settings -> defaults\n", count);
    ctx.r3.u64 = finish(ov, 0);
}
static void XamUserWriteProfileSettings_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = finish(ctx.r8.u32, 0); }

// ======================================================================== enumerators
struct Enumerator { std::vector<std::vector<uint8_t>> items; size_t next = 0; uint32_t itemSize = 0; uint32_t priv = 0; };
static std::mutex g_enLock;
static std::unordered_map<uint32_t, Enumerator> g_enums;
static uint32_t enum_create(Enumerator e) {
    uint32_t h = handle_create(ob_create(0, 1, 0x10));
    std::lock_guard<std::mutex> g(g_enLock); g_enums[h] = std::move(e);
    return h;
}
// XamEnumerate(handle, flags, buffer, size, items_returned*, overlapped*)
static void XamEnumerate_(PPCContext& ctx, uint8_t*) {
    uint32_t h = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32, out = ctx.r7.u32, ov = ctx.r8.u32, n = 0;
    {   std::lock_guard<std::mutex> g(g_enLock);
        auto it = g_enums.find(h);
        if (it != g_enums.end()) {
            Enumerator& e = it->second;
            uint32_t fit = e.itemSize ? size / e.itemSize : 0;
            while (n < fit && e.next < e.items.size()) { memcpy(g_base + buf + n * e.itemSize, e.items[e.next].data(), e.itemSize); e.next++; n++; }
        } }
    if (out) wr32(out, n);
    ctx.r3.u64 = finish(ov, n ? 0 : ERROR_NO_MORE_FILES, n);
}
// XamCreateEnumeratorHandle(user, app, open_msg, close_msg, extra_size, item_count, flags, handle*)
static void XamCreateEnumeratorHandle_(PPCContext& ctx, uint8_t*) {
    Enumerator e; e.priv = vm_alloc(std::max<uint32_t>(ctx.r7.u32, 16));
    uint32_t h = enum_create(std::move(e));
    wr32(ctx.r10.u32, h);
    ctx.r3.u64 = 0;
}
static void XamGetPrivateEnumStructureFromHandle_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_enLock);
    auto it = g_enums.find(ctx.r3.u32);
    if (it == g_enums.end()) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    wr32(ctx.r4.u32, it->second.priv);
    ctx.r3.u64 = 0;
}
static void empty_enumerator(PPCContext& ctx, uint32_t sizeOut, uint32_t handleOut, uint32_t itemSize) {
    Enumerator e; e.itemSize = itemSize;
    uint32_t h = enum_create(std::move(e));
    if (sizeOut) wr32(sizeOut, itemSize * 4);
    if (handleOut) wr32(handleOut, h);
    ctx.r3.u64 = 0;
}
// XamUserCreateAchievementEnumerator(title, user, xuid, flags, offset, count, size*, handle*)
static void XamUserCreateAchievementEnumerator_(PPCContext& ctx, uint8_t*) { empty_enumerator(ctx, ctx.r9.u32, ctx.r10.u32, 0x24); }
// XamUserCreateStatsEnumerator(title, user, xuid?, ... size*, handle* on the stack)
static void XamUserCreateStatsEnumerator_(PPCContext& ctx, uint8_t*) { empty_enumerator(ctx, ctx.r9.u32, ctx.r10.u32, 0x10); }

// ======================================================================== content: saves and DLC
static std::string saves_dir() { std::string s = data_root() + "\\saves"; CreateDirectoryA(s.c_str(), nullptr); return s; }
static std::string dlc_dir() { return data_root() + "\\game\\dlc"; }
static std::vector<uint8_t> content_item(uint32_t type, const std::string& file, const std::wstring& display) {
    std::vector<uint8_t> it(kContentDataSize, 0);
    *(uint32_t*)&it[0] = _byteswap_ulong(kDevice);
    *(uint32_t*)&it[4] = _byteswap_ulong(type);
    for (size_t k = 0; k < display.size() && k < 127; k++) *(uint16_t*)&it[8 + k * 2] = _byteswap_ushort((uint16_t)display[k]);
    memcpy(&it[0x108], file.data(), std::min<size_t>(file.size(), 42));
    return it;
}
static std::wstring utf8_to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::vector<std::vector<uint8_t>> list_content(uint32_t type) {
    std::vector<std::vector<uint8_t>> out;
    const bool dlc = type == 2;
    const std::string root = dlc ? dlc_dir() : saves_dir();
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == "." || n == ".." || n.size() > 42) continue;
        std::vector<uint8_t> item;
        if (dlc) {
            std::ifstream f(root + "\\" + n + ".name"); std::string disp; std::getline(f, disp);
            item = content_item(2, n, utf8_to_wide(disp.empty() ? n : disp));
        } else {
            item.assign(kContentDataSize, 0);
            std::ifstream f(root + "\\" + n + ".xcontent", std::ios::binary);
            if (!f.read((char*)item.data(), kContentDataSize)) item = content_item(type, n, utf8_to_wide(n));
            *(uint32_t*)&item[0] = _byteswap_ulong(kDevice);
            if (_byteswap_ulong(*(uint32_t*)&item[4]) != type) continue;
        }
        out.push_back(std::move(item));
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    return out;
}
// XamContentCreateEnumerator(user, device, type, flags, items, size*, handle*)
static void XamContentCreateEnumerator_(PPCContext& ctx, uint8_t*) {
    uint32_t dev = ctx.r4.u32, type = ctx.r5.u32, items = ctx.r7.u32;
    Enumerator e; e.itemSize = kContentDataSize;
    if (dev == kDevice || dev == 0) e.items = list_content(type);
    LOG("[content] enumerate type=%08X device=%u -> %zu items lr=%08X\n", type, dev, e.items.size(), (uint32_t)ctx.lr);
    uint32_t h = enum_create(std::move(e));
    if (ctx.r8.u32) wr32(ctx.r8.u32, std::max<uint32_t>(items, 1) * kContentDataSize);
    if (ctx.r9.u32) wr32(ctx.r9.u32, h);
    ctx.r3.u64 = 0;
}
static bool content_folder(uint32_t data, std::string& dir, bool& dlc) {
    char nm[43] = {}; memcpy(nm, g_base + data + 0x108, 42);
    std::string f = nm;
    if (f.empty() || f.find("..") != std::string::npos || f.find_first_of("\\/:*?\"<>|") != std::string::npos) return false;
    dlc = rd32(data + 4) == 2;
    dir = (dlc ? dlc_dir() : saves_dir()) + "\\" + f;
    return true;
}
// XamContentCreateEx(user, root, data*, flags, disposition*, license*, cache_size, content_size, overlapped* [stack])
// flags & 0xF: 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing
static void XamContentCreateEx_(PPCContext& ctx, uint8_t*) {
    std::string root = guest_cstr(ctx.r4.u32, 64);
    uint32_t data = ctx.r5.u32, flags = ctx.r6.u32, dispOut = ctx.r7.u32, licOut = ctx.r8.u32, ov = rd32(ctx.r1.u32 + 0x54), mode = flags & 0xF;
    std::string dir; bool dlc = false; uint32_t status = 0, disp = 0;
    if (!content_folder(data, dir, dlc)) status = 87;
    else {
        bool exists = GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (dlc) {
            if (!exists) status = 2; else { disp = 2; content_mount(root, dir, false); }
        } else if (mode == 1 && exists) status = 183;
        else if ((mode == 3 || mode == 5) && !exists) status = 2;
        else {
            if (exists && (mode == 2 || mode == 5)) { std::error_code ec; std::filesystem::remove_all(dir, ec); exists = false; }
            if (!exists) CreateDirectoryA(dir.c_str(), nullptr);
            disp = exists ? 2 : 1;
            std::string meta = dir + ".xcontent";
            if (!exists || GetFileAttributesA(meta.c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::ofstream f(meta, std::ios::binary); f.write((const char*)gp(data), kContentDataSize);
            }
            content_mount(root, dir, true);
        }
    }
    LOG("[content] create root=%s folder=%s flags=%X -> %u lr=%08X\n", root.c_str(), dir.c_str(), flags, status, (uint32_t)ctx.lr);
    if (dispOut) wr32(dispOut, disp);
    if (licOut) wr32(licOut, 0xFFFFFFFF);
    ctx.r3.u64 = finish(ov, status);
}
static void XamContentClose_(PPCContext& ctx, uint8_t*) { content_unmount(guest_cstr(ctx.r3.u32, 64)); ctx.r3.u64 = finish(ctx.r4.u32, 0); }
static void XamContentDelete_(PPCContext& ctx, uint8_t*) {
    std::string dir; bool dlc; uint32_t st = 87;
    if (content_folder(ctx.r4.u32, dir, dlc) && !dlc) { std::error_code ec; st = std::filesystem::remove_all(dir, ec) ? 0 : 2; std::filesystem::remove(dir + ".xcontent", ec); }
    ctx.r3.u64 = finish(ctx.r5.u32, st);
}
static void XamContentGetCreator_(PPCContext& ctx, uint8_t*) {
    std::string dir; bool dlc; uint32_t st = 2;
    if (content_folder(ctx.r4.u32, dir, dlc) && GetFileAttributesA(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        st = 0;
        if (ctx.r5.u32) wr32(ctx.r5.u32, dlc ? 0 : 1);
        if (ctx.r6.u32) wr64(ctx.r6.u32, dlc ? 0 : kXuid);
    }
    ctx.r3.u64 = finish(ctx.r7.u32, st);
}
static void XamContentGetDeviceState_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = finish(ctx.r4.u32, ctx.r3.u32 == kDevice ? 0 : ERROR_DEVICE_NOT_CONNECTED); }
// XamContentGetDeviceData(device, X_CONTENT_DEVICE_DATA*): {u32 id; u32 type; u64 total; u64 free; wchar name[28]}
static void XamContentGetDeviceData_(PPCContext& ctx, uint8_t*) {
    uint32_t out = ctx.r4.u32;
    if (ctx.r3.u32 != kDevice) { ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED; return; }
    memset(g_base + out, 0, 0x50);
    wr32(out, kDevice); wr32(out + 4, 1); wr64(out + 8, 0x400000000ull); wr64(out + 16, 0x200000000ull);
    const char* n = "Hard Drive"; for (int k = 0; n[k]; k++) wr16(out + 0x18 + k * 2, (uint16_t)n[k]);
    ctx.r3.u64 = 0;
}
static void XamContentGetLicenseMask_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr32(ctx.r3.u32, 0xFFFFFFFF); ctx.r3.u64 = finish(ctx.r4.u32, 0); }
static void notify_system_ui();
// XamShowDeviceSelectorUI(user, type, flags, bytes, device*, overlapped*): picks the hard drive
static void XamShowDeviceSelectorUI_(PPCContext& ctx, uint8_t*) {
    if (ctx.r7.u32) wr32(ctx.r7.u32, kDevice);
    ctx.r3.u64 = finish(ctx.r8.u32, 0);
    notify_system_ui();
}

// ======================================================================== notifications
// A listener is a handle whose object is signalled while its queue holds notifications.
struct Listener { uint64_t mask; std::deque<std::pair<uint32_t, uint32_t>> q; uint32_t obj; };
static std::mutex g_ntLock;
static std::unordered_map<uint32_t, Listener> g_listeners;
static void XamNotifyCreateListener_(PPCContext& ctx, uint8_t*) {
    uint32_t obj = ob_create(0, 0, 0x10), h = handle_create(obj);
    Listener l{ ctx.r3.u64, {}, obj };
    if (l.mask & 1) {   // system area: the profile is signed in, the storage device is present, the guide is closed
        l.q.push_back({ 0x0000000A, 1 });   // XN_SYS_SIGNINCHANGED
        l.q.push_back({ 0x0000000B, 0 });   // XN_SYS_STORAGEDEVICESCHANGED
        l.q.push_back({ 0x00000009, 0 });   // XN_SYS_UI
    }
    if (!l.q.empty()) { wr32(obj + 4, 1); ob_signal_changed(); }
    std::lock_guard<std::mutex> g(g_ntLock); g_listeners[h] = l;
    LOG("[xam] notification listener %08X mask=%llX\n", h, (unsigned long long)l.mask);
    ctx.r3.u64 = h;
}
// XNotifyGetNext(handle, match_id, id*, param*)
static void XNotifyGetNext_(PPCContext& ctx, uint8_t*) {
    std::lock_guard<std::mutex> g(g_ntLock);
    auto it = g_listeners.find(ctx.r3.u32);
    if (it == g_listeners.end()) { ctx.r3.u64 = 0; return; }
    auto& q = it->second.q;
    uint32_t match = ctx.r4.u32;
    for (auto i = q.begin(); i != q.end(); ++i) {
        if (match && i->first != match) continue;
        if (ctx.r5.u32) wr32(ctx.r5.u32, i->first);
        if (ctx.r6.u32) wr32(ctx.r6.u32, i->second);
        LOG("[xam] notification %08X param %08X\n", i->first, i->second);
        q.erase(i);
        if (q.empty()) wr32(it->second.obj + 4, 0);
        ctx.r3.u64 = 1; return;
    }
    ctx.r3.u64 = 0;
}
// Every system dialog the game opens is answered at once, but the game still has to hear that the system UI opened and closed:
// UE3 ignores pad input while it believes the guide or a system dialog is up (XN_SYS_UI 1 ... 0). Without this the main menu
// stopped responding after the "saved data does not exist" message box.
static void notify_system_ui() {
    std::lock_guard<std::mutex> g(g_ntLock);
    for (auto& [h, l] : g_listeners) {
        if (!(l.mask & 1)) continue;
        l.q.push_back({ 0x00000009, 1 });
        l.q.push_back({ 0x00000009, 0 });
        wr32(l.obj + 4, 1);
    }
    ob_signal_changed();
}

// ======================================================================== system UI
static void XamShowMessageBoxUI_(PPCContext& ctx, uint8_t*) {
    // (user, title, text, button_count, buttons*, focus, flags, result*, overlapped* [stack]): the first button is chosen
    std::string text; for (uint32_t p = ctx.r5.u32; p && rd16(p); p += 2) text += (char)rd16(p);
    fprintf(stderr, "[ui] message box: %s\n", text.c_str());
    if (ctx.r10.u32) wr32(ctx.r10.u32, 0);
    ctx.r3.u64 = finish(rd32(ctx.r1.u32 + 0x54), 0);
    notify_system_ui();
}
static void XamShowMessageBoxUIEx_(PPCContext& ctx, uint8_t* b) { XamShowMessageBoxUI_(ctx, b); }
static void XamShowKeyboardUI_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = finish(rd32(ctx.r1.u32 + 0x54), 1223); notify_system_ui(); }   // ERROR_CANCELLED
static void XamShowSigninUI_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; notify_system_ui(); }
static void XamShowDirtyDiscErrorUI_(PPCContext& ctx, uint8_t*) {
    fprintf(stderr, "[ui] dirty disc error (a game file could not be read) lr=%08X\n", (uint32_t)ctx.lr); fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 6);
}
static void UiUnavailable_(PPCContext& ctx, uint8_t*) { LOG("[ui] system UI not available lr=%08X\n", (uint32_t)ctx.lr); ctx.r3.u64 = 0; }

// ======================================================================== messages, loader, tasks, misc
// XMsgInProcessCall(app, message, buffer*, size)
static void XMsgInProcessCall_(PPCContext& ctx, uint8_t*) {
    uint32_t app = ctx.r3.u32, msg = ctx.r4.u32, buf = ctx.r5.u32;
    LOG("[xmsg] in-process app=%X msg=%08X buf=%08X\n", app, msg, buf);
    if (app == 0xFA && msg == 0x0007001B && buf) {   // XMP GetPlaybackController: the title controls the music
        if (uint32_t c = rd32(buf + 4)) wr32(c, 0);
        if (uint32_t l = rd32(buf + 8)) wr32(l, 0);
        ctx.r3.u64 = 0; return;
    }
    // other media player calls are accepted and ignored; anything else fails. The result is an HRESULT: a positive Win32 code reads as success.
    ctx.r3.u64 = (app == 0xFA) ? 0 : 0x80070490u;   // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
    if (app != 0xFA) fprintf(stderr, "[xmsg] unsupported in-process call app=%X msg=%08X lr=%08X\n", app, msg, (uint32_t)ctx.lr);
}
static void xmsg_start(PPCContext& ctx, uint32_t app, uint32_t msg, uint32_t ov, uint32_t buf, uint32_t size) {
    uint32_t st = (app == 0xFB || app == 0xFA || app == 0xF7) ? 0 : ERROR_NOT_FOUND;
    LOG("[xmsg] start app=%X msg=%08X buf=%08X size=%X -> %X\n", app, msg, buf, size, st);
    ctx.r3.u64 = finish(ov, st);
}
static void XMsgStartIORequest_(PPCContext& ctx, uint8_t*) { xmsg_start(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
static void XMsgStartIORequestEx_(PPCContext& ctx, uint8_t*) { xmsg_start(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
static void XMsgCompleteIORequest_(PPCContext& ctx, uint8_t*) { complete_overlapped(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32); wr32(ctx.r3.u32 + 0x18, ctx.r5.u32); ctx.r3.u64 = 0; }
// XamGetOverlappedResult(overlapped*, length*, wait)
static void XamGetOverlappedResult_(PPCContext& ctx, uint8_t*) {
    uint32_t ov = ctx.r3.u32, st = rd32(ov);
    if (st == ERROR_IO_PENDING && ctx.r5.u32) {
        if (uint32_t ev = rd32(ov + 0x0C)) { uint32_t o = handle_object(ev); if (o) ke_wait(&o, 1, true, 0, false); }
        st = rd32(ov);
    }
    if (ctx.r4.u32) wr32(ctx.r4.u32, rd32(ov + 4));
    ctx.r3.u64 = st;
}
static void XamLoaderGetLaunchDataSize_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr32(ctx.r3.u32, 0); ctx.r3.u64 = ERROR_NOT_FOUND; }
static void XamLoaderGetLaunchData_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = ERROR_NOT_FOUND; }
static void XamLoaderLaunchTitle_(PPCContext& ctx, uint8_t*) { fprintf(stderr, "[exit] title asked to leave\n"); fflush(stderr); TerminateProcess(GetCurrentProcess(), 0); }
// XamTaskSchedule(callback, context, unknown, handle*): runs callback(context) on a new guest thread
static void XamTaskSchedule_(PPCContext& ctx, uint8_t*) {
    uint32_t cb = ctx.r3.u32, arg = ctx.r4.u32, hp = ctx.r6.u32;
    uint32_t obj = ob_create(0, 0, 0x10), h = handle_create(obj);
    if (hp) wr32(hp, h);
    LOG("[xam] task %08X(%08X)\n", cb, arg);
    std::thread([cb, arg, obj] {
        GuestHostThread t = make_host_guest_thread("xam task");
        PPCContext c{}; void guest_fp_init(PPCContext&); guest_fp_init(c);
        bind_guest_thread(c, t); c.r3.u64 = arg;
        call_guest(c, cb);
        wr32(obj + 4, 1); ob_signal_changed();
    }).detach();
    ctx.r3.u64 = 0;
}
static void XamTaskCloseHandle_(PPCContext& ctx, uint8_t*) { handle_close(ctx.r3.u32); ctx.r3.u64 = 0; }
// XamAlloc(flags, size, out*)
static void XamAlloc_(PPCContext& ctx, uint8_t*) {
    uint32_t a = (ctx.r3.u32 & 0x00100000) ? phys_alloc(ctx.r4.u32, 0x1000) : vm_alloc(ctx.r4.u32);
    if (ctx.r5.u32) wr32(ctx.r5.u32, a);
    ctx.r3.u64 = a ? 0 : 0x8007000E;
}
uint32_t g_executionInfoGuest = 0;   // main.cpp: the XEX execution info header in guest memory
static void XamGetExecutionId_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) wr32(ctx.r3.u32, g_executionInfoGuest); ctx.r3.u64 = 0; }
static void XamGetSystemVersion_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x20096B00; }
static void XGetAVPack_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 6; }
static void XGetLanguage_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
static void XGetGameRegion_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0xFFFF; }
// XGetVideoMode(XVIDEO_MODE*)
static void XGetVideoMode_(PPCContext& ctx, uint8_t*) {
    uint32_t p = ctx.r3.u32;
    if (!p) return;
    memset(g_base + p, 0, 0x30);
    wr32(p, 1280); wr32(p + 4, 720); wr32(p + 8, 0); wr32(p + 0xC, 1); wr32(p + 0x10, 1); wr32(p + 0x14, 0x42700000);   // 60.0f
    wr32(p + 0x18, 1); wr32(p + 0x1C, 0x4A); wr32(p + 0x20, 1);
}
static void XNetLogonGetTitleID_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = kTitleId; }

// ======================================================================== network: offline
static void NetDll_WSAStartup_(PPCContext& ctx, uint8_t*) {
    if (uint32_t p = ctx.r5.u32) { memset(g_base + p, 0, 400); wr16(p, 0x0202); wr16(p + 2, 0x0202); }
    ctx.r3.u64 = 0;
}
static void NetDll_XNetRandom_(PPCContext& ctx, uint8_t*) {
    static std::mt19937 rng{ std::random_device{}() };
    for (uint32_t i = 0; i < ctx.r5.u32; i++) g_base[ctx.r4.u32 + i] = (uint8_t)rng();
    ctx.r3.u64 = 0;
}
static void NetDll_WSACreateEvent_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = handle_create(ob_create(0, 0, 0x10)); }
static void NetDll_inet_addr_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0xFFFFFFFF; }
static void NetDll_XNetGetTitleXnAddr_(PPCContext& ctx, uint8_t*) { if (ctx.r4.u32) memset(g_base + ctx.r4.u32, 0, 0x24); ctx.r3.u64 = 1; }   // XNET_GET_XNADDR_PENDING... none
static void SocketError_(PPCContext& ctx, uint8_t*) { ctx.r3.s64 = -1; }
static void WSAError_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 10050; }   // WSAENETDOWN
static void Zero_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
static void NotFound_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = ERROR_NOT_FOUND; }
static void Failure_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0x80004005; }

void register_xam_imports() {
    register_import("XamInputGetState", XamInputGetState_);
    register_import("XamInputSetState", XamInputSetState_);
    register_import("XamInputGetCapabilities", XamInputGetCapabilities_);

    register_import("XamUserGetSigninState", XamUserGetSigninState_);
    register_import("XamUserGetXUID", XamUserGetXUID_);
    register_import("XamUserGetName", XamUserGetName_);
    register_import("XamUserGetSigninInfo", XamUserGetSigninInfo_);
    register_import("XamUserCheckPrivilege", XamUserCheckPrivilege_);
    register_import("XamUserAreUsersFriends", XamUserAreUsersFriends_);
    register_import("XamUserGetMembershipTierFromXUID", XamUserGetMembershipTierFromXUID_);
    register_import("XamUserGetOnlineCountryFromXUID", XamUserGetOnlineCountryFromXUID_);
    register_import("XamUserReadProfileSettings", XamUserReadProfileSettings_);
    register_import("XamUserWriteProfileSettings", XamUserWriteProfileSettings_);
    register_import("XamUserGetDeviceContext", Zero_);

    register_import("XamEnumerate", XamEnumerate_);
    register_import("XamCreateEnumeratorHandle", XamCreateEnumeratorHandle_);
    register_import("XamGetPrivateEnumStructureFromHandle", XamGetPrivateEnumStructureFromHandle_);
    register_import("XamUserCreateAchievementEnumerator", XamUserCreateAchievementEnumerator_);
    register_import("XamUserCreateStatsEnumerator", XamUserCreateStatsEnumerator_);

    register_import("XamContentCreateEnumerator", XamContentCreateEnumerator_);
    register_import("XamContentCreateEx", XamContentCreateEx_);
    register_import("XamContentClose", XamContentClose_);
    register_import("XamContentDelete", XamContentDelete_);
    register_import("XamContentGetCreator", XamContentGetCreator_);
    register_import("XamContentGetDeviceState", XamContentGetDeviceState_);
    register_import("XamContentGetDeviceData", XamContentGetDeviceData_);
    register_import("XamContentGetLicenseMask", XamContentGetLicenseMask_);
    register_import("XamShowDeviceSelectorUI", XamShowDeviceSelectorUI_);

    register_import("XamNotifyCreateListener", XamNotifyCreateListener_);
    register_import("XNotifyGetNext", XNotifyGetNext_);
    register_import("XNotifyPositionUI", Zero_);

    register_import("XamShowMessageBoxUI", XamShowMessageBoxUI_);
    register_import("XamShowMessageBoxUIEx", XamShowMessageBoxUIEx_);
    register_import("XamShowKeyboardUI", XamShowKeyboardUI_);
    register_import("XamShowSigninUI", XamShowSigninUI_);
    register_import("XamShowDirtyDiscErrorUI", XamShowDirtyDiscErrorUI_);
    for (const char* n : { "XamShowFriendsUI", "XamShowMessagesUI", "XamShowQuickChatUI", "XamShowAchievementsUI", "XamShowPlayerReviewUI",
                           "XamShowMarketplaceUI", "XamShowPlayersUI", "XamShowMessageComposeUI", "XamShowGameInviteUI", "XamShowFriendRequestUI",
                           "XamShowGamerCardUIForXUID", "XamShowCustomMessageComposeUI", "XamShowCustomPlayerListUI" })
        register_import(n, UiUnavailable_);

    register_import("XMsgInProcessCall", XMsgInProcessCall_);
    register_import("XMsgStartIORequest", XMsgStartIORequest_);
    register_import("XMsgStartIORequestEx", XMsgStartIORequestEx_);
    register_import("XMsgCompleteIORequest", XMsgCompleteIORequest_);
    register_import("XMsgCancelIORequest", Zero_);
    register_import("XamGetOverlappedResult", XamGetOverlappedResult_);
    register_import("XamLoaderGetLaunchDataSize", XamLoaderGetLaunchDataSize_);
    register_import("XamLoaderGetLaunchData", XamLoaderGetLaunchData_);
    register_import("XamLoaderLaunchTitle", XamLoaderLaunchTitle_);
    register_import("XamLoaderTerminateTitle", XamLoaderLaunchTitle_);
    register_import("XamTaskSchedule", XamTaskSchedule_);
    register_import("XamTaskCloseHandle", XamTaskCloseHandle_);
    register_import("XamTaskShouldExit", Zero_);
    register_import("XamAlloc", XamAlloc_);
    register_import("XamFree", Zero_);
    register_import("XamGetExecutionId", XamGetExecutionId_);
    register_import("XamGetSystemVersion", XamGetSystemVersion_);
    register_import("XGetAVPack", XGetAVPack_);
    register_import("XGetLanguage", XGetLanguage_);
    register_import("XGetGameRegion", XGetGameRegion_);
    register_import("XGetVideoMode", XGetVideoMode_);
    register_import("XNetLogonGetTitleID", XNetLogonGetTitleID_);
    register_import("XamVoiceCreate", Failure_);
    register_import("XamVoiceHeadsetPresent", Zero_);
    register_import("XamVoiceSubmitPacket", Zero_);
    register_import("XamVoiceClose", Zero_);
    register_import("XamVoiceIsActiveProcess", Zero_);
    register_import("XamSessionCreateHandle", Failure_);
    register_import("XamSessionRefObjByHandle", Failure_);
    register_import("XamWriteGamerTile", NotFound_);
    register_import("XamReadTileToTexture", NotFound_);

    register_import("NetDll_WSAStartup", NetDll_WSAStartup_);
    register_import("NetDll_WSACleanup", Zero_);
    register_import("NetDll_XNetStartup", Zero_);
    register_import("NetDll_XNetCleanup", Zero_);
    register_import("NetDll_XNetRandom", NetDll_XNetRandom_);
    register_import("NetDll_WSACreateEvent", NetDll_WSACreateEvent_);
    register_import("NetDll_WSAGetLastError", WSAError_);
    register_import("NetDll_WSASetLastError", Zero_);
    register_import("NetDll_inet_addr", NetDll_inet_addr_);
    register_import("NetDll_XNetGetTitleXnAddr", NetDll_XNetGetTitleXnAddr_);
    register_import("NetDll_XNetGetEthernetLinkStatus", Zero_);   // no link
    register_import("NetDll_closesocket", Zero_);
    register_import("NetDll_XNetSetSystemLinkPort", Zero_);
    for (const char* n : { "NetDll_socket", "NetDll_ioctlsocket", "NetDll_setsockopt", "NetDll_getsockopt", "NetDll_getsockname", "NetDll_bind",
                           "NetDll_connect", "NetDll_listen", "NetDll_accept", "NetDll_select", "NetDll_recv", "NetDll_recvfrom", "NetDll_send", "NetDll_sendto" })
        register_import(n, SocketError_);
    for (const char* n : { "NetDll_XNetRegisterKey", "NetDll_XNetUnregisterKey", "NetDll_XNetXnAddrToInAddr", "NetDll_XNetServerToInAddr",
                           "NetDll_XNetInAddrToXnAddr", "NetDll_XNetUnregisterInAddr", "NetDll_XNetConnect", "NetDll_XNetDnsLookup", "NetDll_XNetDnsRelease",
                           "NetDll_XNetQosListen", "NetDll_XNetQosLookup", "NetDll_XNetQosRelease", "NetDll_XNetQosGetListenStats" })
        register_import(n, WSAError_);
}
