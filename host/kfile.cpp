// xboxkrnl file system. game:\, d:\ and \Device\Cdrom0\ are the unpacked disc (game\disc, read only). Mounted content roots
// (saves, DLC) come from XamContentCreateEx; symbolic links from ObCreateSymbolicLink. Ported from the BCR2 host.
#include "kernel.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }

std::string data_root() {
    static const std::string r = [] {
        if (const char* e = getenv("ASURA_ROOT")) return std::string(e);
        char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
        std::string d = p; d = d.substr(0, d.find_last_of('\\'));
        for (int up = 0; up < 3; up++) {                         // the folder holding game\disc: next to the exe, or above it (host\build\asura.exe)
            if (GetFileAttributesA((d + "\\game\\disc").c_str()) != INVALID_FILE_ATTRIBUTES) return d;
            d = d.substr(0, d.find_last_of('\\'));
        }
        return std::string("C:\\dev\\asura");
    }();
    return r;
}

struct Mount { std::string host; bool writable; };
static std::mutex g_fsLock;
static std::map<std::string, Mount> g_mounts;              // lower-case "name:" -> folder
static std::map<std::string, std::string> g_links;         // lower-case "name:" -> target device path

void content_mount(const std::string& root, const std::string& dir, bool writable) {
    std::lock_guard<std::mutex> g(g_fsLock);
    std::string k = lower(root);
    if (k.empty() || k.back() != ':') k += ':';
    g_mounts[k] = { dir, writable };
    LOG("[fs] mount %s -> %s%s\n", k.c_str(), dir.c_str(), writable ? "" : " (read only)");
}
void content_unmount(const std::string& root) {
    std::lock_guard<std::mutex> g(g_fsLock);
    std::string k = lower(root);
    if (k.empty() || k.back() != ':') k += ':';
    g_mounts.erase(k);
}

static bool strip_prefix(const std::string& low, const char* pre, size_t& n) { n = strlen(pre); return low.compare(0, n, pre) == 0; }

bool resolve_game_path(const std::string& inRaw, std::string& out, bool& writable) {
    std::string in = inRaw;
    std::replace(in.begin(), in.end(), '/', '\\');
    if (in.find("..\\") != std::string::npos) return false;
    std::string low = lower(in);
    size_t n;
    if (strip_prefix(low, "\\??\\", n)) { in = in.substr(n); low = low.substr(n); }
    std::lock_guard<std::mutex> g(g_fsLock);
    for (int depth = 0; depth < 4; depth++) {   // follow symbolic links
        size_t colon = low.find(':');
        if (colon == std::string::npos || colon > 32) break;
        auto it = g_links.find(low.substr(0, colon + 1));
        if (it == g_links.end()) break;
        in = it->second + in.substr(colon + 1); low = lower(in);
    }
    static const char* disc[] = { "game:\\", "d:\\", "\\device\\cdrom0\\", "game:", "d:", "\\device\\cdrom0" };
    for (const char* pre : disc) if (strip_prefix(low, pre, n)) {
        std::string rest = in.substr(n);
        while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
        out = data_root() + "\\game\\disc" + (rest.empty() ? "" : "\\" + rest);
        writable = false;
        return true;
    }
    if (strip_prefix(low, "update:", n)) {   // title update content: this TU ships only the executable patch, so an empty folder
        std::string rest = in.substr(n);
        while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
        out = data_root() + "\\game\\update" + (rest.empty() ? "" : "\\" + rest);
        writable = false;
        return true;
    }
    size_t colon = low.find(':');
    if (colon != std::string::npos) {
        auto it = g_mounts.find(low.substr(0, colon + 1));
        if (it != g_mounts.end()) {
            std::string rest = in.substr(colon + 1);
            while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
            out = rest.empty() ? it->second.host : it->second.host + "\\" + rest;
            writable = it->second.writable;
            return true;
        }
    }
    return false;
}

// ObCreateSymbolicLink(ANSI_STRING* link, ANSI_STRING* target)
static void ObCreateSymbolicLink_(PPCContext& ctx, uint8_t*) {
    std::string link = lower(guest_ansi_string(ctx.r3.u32)), target = guest_ansi_string(ctx.r4.u32);
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    if (link.empty() || link.back() != ':') link += ':';
    std::lock_guard<std::mutex> g(g_fsLock);
    g_links[link] = target;
    LOG("[fs] symbolic link %s -> %s\n", link.c_str(), target.c_str());
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void ObDeleteSymbolicLink_(PPCContext& ctx, uint8_t*) {
    std::string link = lower(guest_ansi_string(ctx.r3.u32));
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    std::lock_guard<std::mutex> g(g_fsLock);
    g_links.erase(link);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- open files ----
struct GuestFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string path;
    bool dir = false, writable = false;
    uint64_t pos = 0;
    std::vector<WIN32_FIND_DATAA> entries; size_t next = 0; bool listed = false;
    std::mutex m;
};
static std::mutex g_filesLock;
static std::unordered_map<uint32_t, std::shared_ptr<GuestFile>> g_files;
static std::shared_ptr<GuestFile> file_of(uint32_t h) {
    std::lock_guard<std::mutex> g(g_filesLock);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second;
}
void file_close(uint32_t h) {
    std::shared_ptr<GuestFile> f;
    { std::lock_guard<std::mutex> g(g_filesLock); auto it = g_files.find(h); if (it == g_files.end()) return; f = it->second; g_files.erase(it); }
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
}
static void set_iosb(uint32_t iosb, uint32_t status, uint32_t info) { if (iosb) { wr32(iosb, status); wr32(iosb + 4, info); } }
static uint64_t ft64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static uint32_t status_of_error(DWORD e) {
    switch (e) {
      case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
      case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
      case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
      case ERROR_ACCESS_DENIED: return STATUS_ACCESS_DENIED;
      default: return STATUS_UNSUCCESSFUL;
    }
}
// the path named by OBJECT_ATTRIBUTES {u32 root_directory; ANSI_STRING* name; u32 attributes}
static std::string oa_path(uint32_t oa) {
    std::string name = guest_ansi_string(rd32(oa + 4));
    uint32_t root = rd32(oa);
    if (root) if (auto f = file_of(root)) return "@" + f->path + (name.empty() ? "" : "\\" + name);   // '@': already a host path
    return name;
}
static bool host_path_of(const std::string& p, std::string& out, bool& writable) {
    if (!p.empty() && p[0] == '@') { out = p.substr(1); writable = false; return true; }
    return resolve_game_path(p, out, writable);
}

// NtCreateFile(handle*, access, attributes*, iosb*, allocation*, file_attributes, share, disposition, options [stack])
// disposition: 0 supersede, 1 open, 2 create, 3 open_if, 4 overwrite, 5 overwrite_if
static void open_file(PPCContext& ctx, uint32_t outH, uint32_t access, uint32_t oa, uint32_t iosb, uint32_t disposition, uint32_t options) {
    std::string name = oa_path(oa), path; bool writable = false;
    if (!host_path_of(name, path, writable)) {
        LOG("[file] '%s' -> not mapped\n", name.c_str());
        set_iosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0); ctx.r3.u64 = STATUS_OBJECT_PATH_NOT_FOUND; return;
    }
    DWORD attr = GetFileAttributesA(path.c_str());
    const bool existed = attr != INVALID_FILE_ATTRIBUTES;
    bool isDir = (options & 1) || (existed && (attr & FILE_ATTRIBUTE_DIRECTORY));
    const bool wantsWrite = disposition != 1 || (access & 0x40000006);   // GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA
    if (!writable && (disposition == 0 || disposition == 2 || disposition == 4 || disposition == 5 || (disposition == 3 && !existed))) {
        uint32_t st = existed ? STATUS_ACCESS_DENIED : STATUS_OBJECT_NAME_NOT_FOUND;
        LOG("[file] '%s' disposition=%u on read-only storage -> %08X\n", name.c_str(), disposition, st);
        set_iosb(iosb, st, 0); ctx.r3.u64 = st; return;
    }
    if (isDir && !existed && writable && disposition != 1) CreateDirectoryA(path.c_str(), nullptr);
    static const DWORD kMode[6] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, OPEN_ALWAYS };
    DWORD mode = isDir ? OPEN_EXISTING : (disposition < 6 ? kMode[disposition] : OPEN_EXISTING);
    DWORD acc = GENERIC_READ | ((writable && wantsWrite && !isDir) ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileA(path.c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, mode, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        uint32_t st = status_of_error(GetLastError());
        LOG("[file] '%s' -> %08X\n", name.c_str(), st);
        set_iosb(iosb, st, 0); ctx.r3.u64 = st; return;
    }
    if (disposition == 5 && existed && !isDir) SetEndOfFile(h);   // overwrite_if
    auto f = std::make_shared<GuestFile>();
    f->h = h; f->path = path; f->dir = isDir; f->writable = writable;
    uint32_t gh = handle_create(ob_create(0, 1, 0x10));
    { std::lock_guard<std::mutex> g(g_filesLock); g_files[gh] = f; }
    wr32(outH, gh);
    uint32_t info = !existed ? 2 : (disposition == 0 ? 0 : (disposition >= 4 ? 3 : 1));   // created / superseded / overwritten / opened
    set_iosb(iosb, STATUS_SUCCESS, info);
    LOG("[file] open '%s'%s -> %08X\n", name.c_str(), isDir ? " (dir)" : "", gh);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtCreateFile_(PPCContext& ctx, uint8_t*) {
    open_file(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, rd32(ctx.r1.u32 + 0x54));
}
// NtOpenFile(handle*, access, attributes*, iosb*, share, options)
static void NtOpenFile_(PPCContext& ctx, uint8_t*) {
    open_file(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

static uint32_t read_at(GuestFile& f, uint8_t* dst, uint32_t len, uint32_t offP, uint32_t& got) {
    std::lock_guard<std::mutex> g(f.m);
    uint64_t off = f.pos;
    if (offP) { uint64_t o = rd64(offP); if (o != 0xFFFFFFFFFFFFFFFEull && o != 0xFFFFFFFFFFFFFFFFull) off = o; }
    OVERLAPPED ov{}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD n = 0;
    BOOL ok = ReadFile(f.h, dst, len, &n, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF) { got = 0; return STATUS_UNSUCCESSFUL; }
    f.pos = off + n; got = n;
    return (n == 0 && len) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
}
static uint32_t write_at(GuestFile& f, const uint8_t* src, uint32_t len, uint32_t offP, uint32_t& put) {
    if (!f.writable) { put = 0; return STATUS_ACCESS_DENIED; }
    std::lock_guard<std::mutex> g(f.m);
    uint64_t off = f.pos;
    if (offP) { uint64_t o = rd64(offP); if (o == 0xFFFFFFFFFFFFFFFFull) { LARGE_INTEGER s; GetFileSizeEx(f.h, &s); off = s.QuadPart; } else if (o != 0xFFFFFFFFFFFFFFFEull) off = o; }
    OVERLAPPED ov{}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD n = 0;
    BOOL ok = WriteFile(f.h, src, len, &n, &ov);
    f.pos = off + n; put = n;
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
// After a synchronous read or write: the event and the APC the caller asked for.
static void io_done(PPCContext&, uint32_t event, uint32_t apc, uint32_t apcCtx, uint32_t iosb) {
    if (event) event_set_by_handle(event);
    if (apc) apc_queue(apc, apcCtx, iosb, 0);   // PIO_APC_ROUTINE(context, iosb, reserved), at the thread's next alertable wait
}
// NtReadFile(handle, event, apc, apc_ctx, iosb, buffer, length, offset*)
static void NtReadFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t ev = ctx.r4.u32, apc = ctx.r5.u32, apcCtx = ctx.r6.u32, iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t got, st = read_at(*f, g_base + buf, len, offP, got);
    set_iosb(iosb, st, got);
    LOG("[file] read %s len=%X -> %X st=%08X\n", f->path.c_str() + data_root().size(), len, got, st);
    io_done(ctx, ev, apc, apcCtx, iosb);
    ctx.r3.u64 = st;
}
// FILE_SEGMENT_ELEMENT: an 8-byte union holding a 32-bit page pointer; take whichever half is set (logged once to settle it)
static uint32_t seg_page(uint32_t seg, uint32_t i) {
    uint32_t hi = rd32(seg + i * 8), lo = rd32(seg + i * 8 + 4);
    static bool once = false;
    if (!once) { once = true; LOG("[file] scatter/gather segment words %08X %08X\n", hi, lo); }
    return hi ? hi : lo;
}
// NtReadFileScatter(handle, event, apc, apc_ctx, iosb, segments*, length, offset*): segments are 4K page addresses
static void NtReadFileScatter_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t ev = ctx.r4.u32, apc = ctx.r5.u32, apcCtx = ctx.r6.u32, iosb = ctx.r7.u32, seg = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::vector<uint8_t> tmp(len);
    uint32_t got, st = read_at(*f, tmp.data(), len, offP, got);
    for (uint32_t o = 0, i = 0; o < got; o += 0x1000, i++) memcpy(g_base + seg_page(seg, i), tmp.data() + o, std::min<uint32_t>(0x1000, got - o));
    set_iosb(iosb, st, got);
    io_done(ctx, ev, apc, apcCtx, iosb);
    ctx.r3.u64 = st;
}
static void NtWriteFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t ev = ctx.r4.u32, apc = ctx.r5.u32, apcCtx = ctx.r6.u32, iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t put, st = write_at(*f, g_base + buf, len, offP, put);
    set_iosb(iosb, st, put);
    io_done(ctx, ev, apc, apcCtx, iosb);
    ctx.r3.u64 = st;
}
static void NtWriteFileGather_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t ev = ctx.r4.u32, apc = ctx.r5.u32, apcCtx = ctx.r6.u32, iosb = ctx.r7.u32, seg = ctx.r8.u32, len = ctx.r9.u32, offP = ctx.r10.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::vector<uint8_t> tmp(len);
    for (uint32_t o = 0, i = 0; o < len; o += 0x1000, i++) memcpy(tmp.data() + o, g_base + seg_page(seg, i), std::min<uint32_t>(0x1000, len - o));
    uint32_t put, st = write_at(*f, tmp.data(), len, offP, put);
    set_iosb(iosb, st, put);
    io_done(ctx, ev, apc, apcCtx, iosb);
    ctx.r3.u64 = st;
}
static void NtFlushBuffersFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    if (f && f->writable) FlushFileBuffers(f->h);
    set_iosb(ctx.r4.u32, STATUS_SUCCESS, 0);
    ctx.r3.u64 = f ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
// FILE_NETWORK_OPEN_INFORMATION (0x38): times, allocation, size, attributes
static void write_network_info(uint32_t o, const WIN32_FILE_ATTRIBUTE_DATA& a) {
    uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    wr64(o + 0x00, ft64(a.ftCreationTime)); wr64(o + 0x08, ft64(a.ftLastAccessTime));
    wr64(o + 0x10, ft64(a.ftLastWriteTime)); wr64(o + 0x18, ft64(a.ftLastWriteTime));
    wr64(o + 0x20, (size + 0xFFF) & ~0xFFFull); wr64(o + 0x28, size);
    wr32(o + 0x30, a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY) ? a.dwFileAttributes & 0x31 : 0x80);
    wr32(o + 0x34, 0);
}
// NtQueryFullAttributesFile(attributes*, FILE_NETWORK_OPEN_INFORMATION*)
static void NtQueryFullAttributesFile_(PPCContext& ctx, uint8_t*) {
    std::string name = oa_path(ctx.r3.u32), path; bool w;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!host_path_of(name, path, w) || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) {
        LOG("[file] attributes '%s' -> not found\n", name.c_str());
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND; return;
    }
    write_network_info(ctx.r4.u32, a);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtQueryInformationFile(handle, iosb, info*, length, class)
static void NtQueryInformationFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    WIN32_FILE_ATTRIBUTE_DATA a{}; GetFileAttributesExA(f->path.c_str(), GetFileExInfoStandard, &a);
    uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    uint32_t n = 0;
    switch (cls) {
      case 4:   // FileBasicInformation
        wr64(info, ft64(a.ftCreationTime)); wr64(info + 8, ft64(a.ftLastAccessTime)); wr64(info + 16, ft64(a.ftLastWriteTime));
        wr64(info + 24, ft64(a.ftLastWriteTime)); wr32(info + 32, f->dir ? 0x10 : 0x80); wr32(info + 36, 0); n = 40; break;
      case 5:   // FileStandardInformation
        wr64(info, (size + 0xFFF) & ~0xFFFull); wr64(info + 8, size); wr32(info + 16, 1); g_base[info + 20] = 0; g_base[info + 21] = f->dir; n = 24; break;
      case 14:  // FilePositionInformation
        wr64(info, f->pos); n = 8; break;
      case 34:  // FileNetworkOpenInformation
        write_network_info(info, a); n = 0x38; break;
      case 22:  // FileModeInformation
        wr32(info, 0); n = 4; break;
      case 26: {  // Xbox FileSectorInformation: the file's first disc sector (UE3 orders its reads by it). A stable per-path number will do.
        uint32_t h = 2166136261u; for (char c : f->path) h = (h ^ (uint8_t)tolower(c)) * 16777619u;
        wr32(info, (h >> 8) & 0x3FFFFF); wr32(info + 4, 0); n = 8; break; }
      case 35:  // FileAttributeTagInformation
        wr32(info, f->dir ? 0x10 : 0x80); wr32(info + 4, 0); n = 8; break;
      default:
        fprintf(stderr, "[file] NtQueryInformationFile class %u unsupported\n", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS; return;
    }
    set_iosb(iosb, STATUS_SUCCESS, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtSetInformationFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t st = STATUS_SUCCESS;
    switch (cls) {
      case 14: { std::lock_guard<std::mutex> g(f->m); f->pos = rd64(info); break; }
      case 19: case 20: {
        if (!f->writable) { st = STATUS_ACCESS_DENIED; break; }
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)rd64(info);
        FILE_END_OF_FILE_INFO e{ li };
        if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e)) st = STATUS_UNSUCCESSFUL;
        break; }
      case 4: break;   // basic information (times): ignored
      case 13:         // disposition: delete on close
        if (!f->writable) st = STATUS_ACCESS_DENIED;
        else { FILE_DISPOSITION_INFO d{ g_base[info] != 0 }; SetFileInformationByHandle(f->h, FileDispositionInfo, &d, sizeof d); }
        break;
      default:
        fprintf(stderr, "[file] NtSetInformationFile class %u unsupported\n", cls);
        st = STATUS_INVALID_INFO_CLASS;
    }
    set_iosb(iosb, st, 0);
    ctx.r3.u64 = st;
}
// NtQueryVolumeInformationFile(handle, iosb, info*, length, class)
static void NtQueryVolumeInformationFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t n = 0;
    if (cls == 1) {        // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        memset(g_base + info, 0, std::min<uint32_t>(len, 0x18));
        wr32(info + 8, 0x12345678); n = 0x12;
    } else if (cls == 3) { // FileFsSizeInformation: total units, available units, sectors per unit, bytes per sector
        wr64(info, 0x100000); wr64(info + 8, f->writable ? 0x80000 : 0); wr32(info + 16, 8); wr32(info + 20, 0x200); n = 24;
    } else if (cls == 5) { // FileFsAttributeInformation: attributes, max name length, name length, name
        const char* fs = f->writable ? "FATX" : "GDFX";
        wr32(info, 0); wr32(info + 4, 255); wr32(info + 8, 4); memcpy(g_base + info + 12, fs, 4); n = 16;
    } else {
        fprintf(stderr, "[file] NtQueryVolumeInformationFile class %u unsupported\n", cls);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS; return;
    }
    set_iosb(iosb, STATUS_SUCCESS, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// NtQueryDirectoryFile(handle, event, apc, apc_ctx, iosb, info*, length, mask*, restart [stack]): FILE_DIRECTORY_INFORMATION, one entry per call
static void NtQueryDirectoryFile_(PPCContext& ctx, uint8_t*) {
    auto f = file_of(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, len = ctx.r9.u32, maskP = ctx.r10.u32;
    bool restart = rd32(ctx.r1.u32 + 0x54) != 0;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> g(f->m);
    if (!f->listed || restart) {
        f->entries.clear(); f->next = 0; f->listed = true;
        std::string mask = maskP ? guest_ansi_string(maskP) : "";
        if (mask.empty()) mask = "*";
        WIN32_FIND_DATAA fd;
        HANDLE fh = FindFirstFileA((f->path + "\\" + mask).c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) f->entries.push_back(fd); } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
    }
    if (f->next >= f->entries.size()) {
        uint32_t st = f->next == 0 ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        set_iosb(iosb, st, 0); ctx.r3.u64 = st; return;
    }
    const WIN32_FIND_DATAA& fd = f->entries[f->next];
    uint32_t nl = (uint32_t)strlen(fd.cFileName);
    if (len < 0x40 + nl) { set_iosb(iosb, STATUS_BUFFER_OVERFLOW, 0); ctx.r3.u64 = STATUS_BUFFER_OVERFLOW; return; }
    uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    wr32(info, 0); wr32(info + 4, (uint32_t)f->next);
    wr64(info + 0x08, ft64(fd.ftCreationTime)); wr64(info + 0x10, ft64(fd.ftLastAccessTime));
    wr64(info + 0x18, ft64(fd.ftLastWriteTime)); wr64(info + 0x20, ft64(fd.ftLastWriteTime));
    wr64(info + 0x28, size); wr64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    wr32(info + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
    wr32(info + 0x3C, nl);
    memcpy(g_base + info + 0x40, fd.cFileName, nl);
    f->next++;
    set_iosb(iosb, STATUS_SUCCESS, 0x40 + nl);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void NtDeviceIoControlFile_(PPCContext& ctx, uint8_t*) {
    LOG("[file] NtDeviceIoControlFile handle=%08X code=%08X lr=%08X\n", ctx.r3.u32, ctx.r8.u32, (uint32_t)ctx.lr);
    ctx.r3.u64 = STATUS_NOT_IMPLEMENTED;
}

void register_file_imports() {
    register_import("NtCreateFile", NtCreateFile_);
    register_import("NtOpenFile", NtOpenFile_);
    register_import("NtReadFile", NtReadFile_);
    register_import("NtReadFileScatter", NtReadFileScatter_);
    register_import("NtWriteFile", NtWriteFile_);
    register_import("NtWriteFileGather", NtWriteFileGather_);
    register_import("NtFlushBuffersFile", NtFlushBuffersFile_);
    register_import("NtQueryFullAttributesFile", NtQueryFullAttributesFile_);
    register_import("NtQueryInformationFile", NtQueryInformationFile_);
    register_import("NtSetInformationFile", NtSetInformationFile_);
    register_import("NtQueryVolumeInformationFile", NtQueryVolumeInformationFile_);
    register_import("NtQueryDirectoryFile", NtQueryDirectoryFile_);
    register_import("NtDeviceIoControlFile", NtDeviceIoControlFile_);
    register_import("ObCreateSymbolicLink", ObCreateSymbolicLink_);
    register_import("ObDeleteSymbolicLink", ObDeleteSymbolicLink_);
}
