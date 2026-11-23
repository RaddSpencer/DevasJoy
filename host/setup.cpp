// Setup for the Asura's Wrath recomp (asura_setup.exe): see setup.h.
//
// How a file is recognised: by its exact size first (each of the seven files has a different size), then by SHA-256. Files that look like one of
// them but have another size (an .iso named "Asura...", a content package of title 43430817) are remembered so the player can be told they have a
// different version. Hashes are cached in game\sources.ini by path, size and modification time.
#include "setup.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <commctrl.h>
#include <shellapi.h>
#include "xex_patcher.h"
#include "inputguard.h"   // thirdparty\xexpatch (XenonRecomp's XEX delta patcher)

#pragma comment(lib, "bcrypt.lib")

namespace {

enum class Need { Required, Recommended, Optional };

struct KnownFile {
    const char* key;            // name in game\sources.ini
    const wchar_t* title;
    const wchar_t* about;       // what it is, for the dialogs
    Need need;
    uint64_t size;
    const char* sha256;         // lowercase hex
    const wchar_t* usualName;   // the file's usual name (DLC and TU packages keep the console's names)
    uint32_t stfsType;          // content type in the package header; 0 for the disc image
};

// The known-good files. Hashes and sizes are from the copies the recomp was built and tested with (the Redump "World" disc image, title update 1
// as the console caches it, the five DLC packages from Xbox Live).
const KnownFile kSetupFiles[] = {
    { "iso", L"Asura's Wrath disc image (ISO)", L"the game itself: its data and its program (default.xex)", Need::Required,
      8738846720ull, "b94db72ab0dcef88e14f47e820ef42502b3ebca246d50d564776e630287fd423", L"Asura's Wrath (World).iso", 0 },
    { "tu", L"Title update 1", L"the update the recompiled program was made from (default.xexp in a TU package)", Need::Required,
      626688ull, "43914d513122c49afa70f051143f4b3b89279e5d9ef05f2ec73abc604c4dc0a0", L"TU_11K620N_000000K000000.0000000000085", 0x000B0000 },
    { "dlc_part4", L"Episode Pack: Part IV (episodes 19-22)", L"DLC: the four episodes after episode 18, which carry the story to its real ending", Need::Recommended,
      1769050112ull, "543e5273cf788cd9c5e47dcc3d629eb8a88f6996f6f46c2269bba800742be787", L"78E95E38A274CA2033EB0517828152A3038C11C943", 2 },
    { "dlc_11_5", L"Episode 11.5", L"DLC: an extra episode between episodes 11 and 12", Need::Optional,
      1163853824ull, "17c6b0f27d9c6d67c6693dd3712f3fb2d9bc2837d226c418302b3141b8c22ae2", L"8A6811EC2255FE171FAF9E4372B23B1B0581E4CA43", 2 },
    { "dlc_15_5", L"Episode 15.5", L"DLC: an extra episode between episodes 15 and 16", Need::Optional,
      1322115072ull, "69c8d489c295086b755ac635a4c9279e394cb893a817e31cffd7580d00efff16", L"E2AFB00ED67E4B55C63F8DA33C0F8783F88E29B843", 2 },
    { "dlc_lost1", L"Lost Episode 1", L"DLC: a side episode", Need::Optional,
      164962304ull, "3a1315c5cbf102da29b9a55ff591be36cfebb2c64b5eade9b116b59eed24e089", L"981ED33639F0A6275BA34BDD2E3D276C16F94A6D43", 2 },
    { "dlc_lost2", L"Lost Episode 2", L"DLC: a side episode", Need::Optional,
      251736064ull, "9fa7e282990404043b94d8f07ae53c8a6457474dd8a8becceb5e69f9d07d5a37", L"F56F8734F04D89EAB07E9003A7F48DE046DA57B743", 2 },
};
constexpr size_t kCount = sizeof(kSetupFiles) / sizeof(kSetupFiles[0]);
constexpr uint32_t kTitleId = 0x43430817;
const wchar_t* const kTitle = L"Asura's Wrath recomp - Setup";

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------------------------------------------------------------------------------------
std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
std::wstring lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring parent(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? L"." : p.substr(0, k); }
std::wstring file_name(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(k + 1); }
std::wstring env(const wchar_t* n) { wchar_t b[MAX_PATH * 2]; DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH * 2); return k && k < MAX_PATH * 2 ? b : L""; }
std::wstring gb(uint64_t bytes) {
    wchar_t b[64];
    if (bytes >= 1000000000ull) swprintf(b, 64, L"%.2f GB", bytes / 1e9); else swprintf(b, 64, L"%.0f MB", bytes / 1e6);
    return b;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool file_stat(const std::wstring& p, uint64_t& size, uint64_t& time) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    time = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

// The folder the recomp's data lives in: as host\kfile.cpp's data_root() (ASURA_ROOT, or the folder holding game\disc next to or above the exe),
// also accepting a folder that only has private\ or gamefiles\ yet. Otherwise the exe's own folder.
std::wstring data_root() {
    std::wstring e = env(L"ASURA_ROOT");
    if (!e.empty()) return e;
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    const std::wstring exeDir = parent(p);
    for (const wchar_t* marker : { L"\\game\\disc", L"\\private", L"\\gamefiles" }) {
        std::wstring d = exeDir;
        for (int up = 0; up < 3 && !d.empty(); up++) {
            if (exists(d + marker)) return d;
            d = parent(d);
        }
    }
    return exeDir;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Progress dialog (the standard Windows one, with Cancel). Without a dialog (the --check mode) it prints a line now and then.
// ------------------------------------------------------------------------------------------------------------------------------------------------
// The setup window's worker reports here instead (set once, used only by that thread): (done, total, line2, line1 or null) -> keep going.
std::function<bool(uint64_t, uint64_t, const std::wstring&, const std::wstring*)> g_progressHook;

struct Progress {
    IProgressDialog* d = nullptr;
    bool console = false, hooked = false;
    DWORD tick = 0;
    Progress(const wchar_t* line1, bool ui) {
        console = !ui;
        if (g_progressHook) { hooked = true; console = false; const std::wstring l = line1; g_progressHook(0, 0, L"", &l); return; }
        if (ui && SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
            d->SetTitle(kTitle);
            d->SetLine(1, line1, FALSE, nullptr);
            d->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
        }
    }
    ~Progress() { if (d) { d->StopProgressDialog(); d->Release(); } }
    void line1(const std::wstring& s) { if (hooked) g_progressHook(0, 0, L"", &s); if (d) d->SetLine(1, s.c_str(), FALSE, nullptr); }
    DWORD printed = 0;
    bool update(uint64_t done, uint64_t total, const std::wstring& line2) {
        if (hooked) { if (GetTickCount() - tick < 100 && done < total) return g_progressHook(UINT64_MAX, 0, L"", nullptr); tick = GetTickCount(); return g_progressHook(done, total, line2, nullptr); }
        if (GetTickCount() - tick < 100) return d ? !d->HasUserCancelled() : true;
        tick = GetTickCount();
        if (!d) {
            if (console && total && GetTickCount() - printed > 2000) { printed = GetTickCount(); fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * done / total, utf8(line2).c_str()); }
            return true;
        }
        d->SetProgress64(done, total ? total : 1);
        d->SetLine(2, line2.c_str(), TRUE, nullptr);
        return !d->HasUserCancelled();
    }
};

// SHA-256 of a file; "" if it cannot be read or the progress callback cancelled.
std::string file_sha256(const std::wstring& path, const std::function<bool(uint64_t, uint64_t)>& progress, bool* cancelled = nullptr) {
    if (cancelled) *cancelled = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; uint8_t out[32] = {}; std::string res;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<uint8_t> buf(1 << 22); uint64_t done = 0; bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
            if (!got) break;
            BCryptHashData(h, buf.data(), got, 0); done += got;
            if (progress && !progress(done, (uint64_t)sz.QuadPart)) { ok = false; if (cancelled) *cancelled = true; break; }
        }
        if (ok && BCryptFinishHash(h, out, 32, 0) == 0) { char hx[3]; for (uint8_t c : out) { snprintf(hx, 3, "%02x", c); res += hx; } }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return res;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// State: what was found, and the hash cache (game\sources.ini)
// ------------------------------------------------------------------------------------------------------------------------------------------------
enum class Status { Unknown, Verified, Skipped };

struct Mismatch { std::wstring path; uint64_t size = 0; std::string sha; };   // sha empty: told apart by size alone

struct Found {
    Status status = Status::Unknown;
    std::wstring path;
    uint64_t time = 0;
    std::vector<Mismatch> mismatches;
    std::vector<std::wstring> candidates;    // files of the right size, not hashed yet
};

struct CacheEntry { uint64_t size = 0, time = 0; std::string sha; };

struct State {
    std::wstring root, sourcesPath;
    Found found[kCount];
    std::map<std::wstring, CacheEntry> cache;   // key: lower-case path
    std::wstring prevPath[kCount];              // where each file was verified last time
    bool ui = true;

    void load() {
        FILE* f = _wfopen(sourcesPath.c_str(), L"rb");
        if (!f) return;
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            // "hash <sha> <size> <time> <path>" and "file <key> <status> <size> <time> <sha> <path>"
            auto tok = [&](size_t& pos) { size_t e = l.find(' ', pos); std::string t = l.substr(pos, e == std::string::npos ? std::string::npos : e - pos); pos = e == std::string::npos ? l.size() : e + 1; return t; };
            size_t pos = 0; std::string kind = tok(pos);
            if (kind == "hash") {
                CacheEntry c; c.sha = tok(pos); c.size = _strtoui64(tok(pos).c_str(), nullptr, 10); c.time = _strtoui64(tok(pos).c_str(), nullptr, 10);
                std::wstring p = wide(l.substr(pos));
                if (c.sha.size() == 64 && !p.empty()) cache[lower(p)] = c;
            } else if (kind == "file") {
                std::string key = tok(pos), st = tok(pos); tok(pos); tok(pos); tok(pos);
                std::wstring p = wide(l.substr(pos));
                for (size_t i = 0; i < kCount; i++) if (key == kSetupFiles[i].key && st == "verified") prevPath[i] = p;
            }
        }
        fclose(f);
    }

    bool save() {
        CreateDirectoryW((root + L"\\game").c_str(), nullptr);
        FILE* f = _wfopen(sourcesPath.c_str(), L"wb");
        if (!f) return false;
        fputs("# Written by asura_setup.exe: the player's own game files it verified, for unpacking into game\\ and private\\.\n"
              "# file <key> <verified|skipped|missing> <size> <mtime> <sha256> <path>; hash lines cache every file hashed. Local to this machine.\n", f);
        for (size_t i = 0; i < kCount; i++) {
            const Found& x = found[i];
            const char* st = x.status == Status::Verified ? "verified" : x.status == Status::Skipped ? "skipped" : "missing";
            fprintf(f, "file %s %s %llu %llu %s %s\n", kSetupFiles[i].key, st, x.status == Status::Verified ? (unsigned long long)kSetupFiles[i].size : 0ull,
                    (unsigned long long)x.time, x.status == Status::Verified ? kSetupFiles[i].sha256 : "-", utf8(x.path).c_str());
        }
        for (auto& [p, c] : cache) fprintf(f, "hash %s %llu %llu %s\n", c.sha.c_str(), (unsigned long long)c.size, (unsigned long long)c.time, utf8(p).c_str());
        fclose(f);
        return true;
    }

    bool allRequired() const {
        for (size_t i = 0; i < kCount; i++) if (kSetupFiles[i].need == Need::Required && found[i].status != Status::Verified) return false;
        return true;
    }
    bool allFound() const {
        for (size_t i = 0; i < kCount; i++) if (found[i].status != Status::Verified) return false;
        return true;
    }

    // SHA-256 through the cache. "" when unreadable or cancelled.
    std::string sha_of(const std::wstring& path, Progress* pr, bool* cancelled) {
        if (cancelled) *cancelled = false;
        uint64_t size = 0, time = 0;
        if (!file_stat(path, size, time)) return "";
        auto it = cache.find(lower(path));
        if (it != cache.end() && it->second.size == size && it->second.time == time) return it->second.sha;
        if (pr) pr->line1(L"Checking " + file_name(path) + L" (" + gb(size) + L")...");
        if (!ui) { fprintf(stderr, "hashing %s (%s)...\n", utf8(path).c_str(), utf8(gb(size)).c_str()); }
        std::string sha = file_sha256(path, [&](uint64_t d, uint64_t t) { return pr ? pr->update(d, t, path) : true; }, cancelled);
        if (!sha.empty()) cache[lower(path)] = { size, time, sha };
        return sha;
    }

    // Checks one file against entry i. Returns true when it is that file.
    bool try_file(size_t i, const std::wstring& path, Progress* pr, bool* cancelled) {
        uint64_t size = 0, time = 0;
        if (!file_stat(path, size, time)) return false;
        Found& x = found[i];
        if (size != kSetupFiles[i].size) { add_mismatch(i, { path, size, "" }); return false; }
        std::string sha = sha_of(path, pr, cancelled);
        if (sha.empty()) return false;
        if (sha != kSetupFiles[i].sha256) { add_mismatch(i, { path, size, sha }); return false; }
        x.status = Status::Verified; x.path = path; x.time = time;
        return true;
    }
    void add_mismatch(size_t i, const Mismatch& m) {
        for (auto& o : found[i].mismatches) if (lower(o.path) == lower(m.path)) { o = m; return; }
        found[i].mismatches.push_back(m);
    }

    // Hashes the candidates collected by a scan, entry by entry, until one verifies.
    bool verify_candidates(Progress* pr) {
        for (size_t i = 0; i < kCount; i++) {
            Found& x = found[i];
            if (x.status == Status::Verified) { x.candidates.clear(); continue; }
            for (const auto& c : x.candidates) {
                bool cancelled = false;
                if (try_file(i, c, pr, &cancelled)) break;
                if (cancelled) return false;
            }
            x.candidates.clear();
        }
        return true;
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Searching folders
// ------------------------------------------------------------------------------------------------------------------------------------------------
bool skip_dir(const std::wstring& name, const std::wstring& full, const State& st) {
    static const wchar_t* const kSkip[] = { L"windows", L"program files", L"program files (x86)", L"programdata", L"$recycle.bin", L"system volume information",
                                            L"appdata", L"node_modules", L".git", L"$windows.~bt", L"$windows.~ws", L"recovery", L"msocache" };
    const std::wstring n = lower(name);
    for (auto* s : kSkip) if (n == s) return true;
    // the recomp's own output folders: the unpacked game holds thousands of files and none of the originals
    const std::wstring f = lower(full), r = lower(st.root);
    for (const wchar_t* own : { L"\\game", L"\\private", L"\\ppc", L"\\logs", L"\\cache", L"\\host", L"\\mods", L"\\replays", L"\\screenshots" })
        if (f == r + own) return true;
    return false;
}

// Looks at one file met while scanning: a file of exactly a known size is a candidate; a near miss (same kind of file, other size) is noted.
void consider(State& st, const std::wstring& path, uint64_t size) {
    for (size_t i = 0; i < kCount; i++) {
        if (st.found[i].status == Status::Verified) continue;
        if (size == kSetupFiles[i].size) {
            auto& c = st.found[i].candidates;
            // the usual name first: it is the likeliest to be the right one
            const bool usual = lower(file_name(path)) == lower(kSetupFiles[i].usualName);
            if (std::find_if(c.begin(), c.end(), [&](const std::wstring& o) { return lower(o) == lower(path); }) == c.end())
                c.insert(usual ? c.begin() : c.end(), path);
            return;
        }
    }
    const std::wstring n = lower(file_name(path));
    if (n.size() > 4 && n.substr(n.size() - 4) == L".iso" && n.find(L"asura") != std::wstring::npos && st.found[0].status != Status::Verified) {
        st.add_mismatch(0, { path, size, "" });
        return;
    }
    // content packages: a 42-character content id (DLC), or a cached title update; check the header's title and type
    const bool looksLikePackage = (n.size() == 42 && n.find(L'.') == std::wstring::npos) || n.rfind(L"tu_", 0) == 0;
    if (!looksLikePackage || size < 0xC000) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    uint8_t hdr[0x400] = {}; DWORD got = 0;
    ReadFile(h, hdr, sizeof hdr, &got, nullptr);
    CloseHandle(h);
    if (got < sizeof hdr || (memcmp(hdr, "LIVE", 4) && memcmp(hdr, "PIRS", 4) && memcmp(hdr, "CON ", 4)) || be32(hdr + 0x360) != kTitleId) return;
    const uint32_t type = be32(hdr + 0x344);
    for (size_t i = 0; i < kCount; i++) {
        if (st.found[i].status == Status::Verified || kSetupFiles[i].stfsType != type) continue;
        if (type == 2 && n != lower(kSetupFiles[i].usualName)) continue;   // a DLC package is named by its content id
        st.add_mismatch(i, { path, size, "" });
    }
}

// Walks a folder tree (depth levels below it), feeding every file to consider(). Returns false when cancelled.
bool scan(State& st, const std::wstring& dir, int depth, Progress* pr, uint64_t& filesSeen) {
    if (pr && !pr->update(0, 0, dir)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return true;
    std::vector<std::wstring> subdirs;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !skip_dir(n, full, st)) subdirs.push_back(full);
        } else {
            filesSeen++;
            consider(st, full, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& s : subdirs) if (!scan(st, s, depth - 1, pr, filesSeen)) return false;
    return true;
}

struct Place { std::wstring dir; int depth; };

// The quick pass: where the files usually are. Xenia keeps title updates and DLC under content\<profile>\43430817\.
std::vector<Place> usual_places(const State& st) {
    std::vector<Place> v;
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    v.push_back({ st.root, 0 });
    v.push_back({ st.root + L"\\gamefiles", 6 });
    v.push_back({ parent(p), 0 });
    const std::wstring home = env(L"USERPROFILE");
    if (!home.empty()) {
        v.push_back({ home + L"\\Documents\\Xenia", 7 });
        v.push_back({ home + L"\\Downloads", 3 });
        v.push_back({ home + L"\\Desktop", 3 });
        v.push_back({ home + L"\\Documents", 3 });
    }
    return v;
}

// Every fixed drive, a few levels deep.
std::vector<Place> all_drives() {
    std::vector<Place> v;
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(mask & (1u << d))) continue;
        std::wstring r = std::wstring(1, (wchar_t)(L'A' + d)) + L":";
        if (GetDriveTypeW((r + L"\\").c_str()) == DRIVE_FIXED) v.push_back({ r, 6 });
    }
    return v;
}

// Scans the places, then hashes the candidates. False when the player cancelled.
bool search(State& st, const std::vector<Place>& places, const wchar_t* what) {
    Progress pr(what, st.ui);
    uint64_t seen = 0;
    for (const auto& pl : places) {
        if (!st.ui) fprintf(stderr, "searching %s\n", utf8(pl.dir).c_str());
        if (!scan(st, pl.dir, pl.depth, &pr, seen)) return false;
    }
    pr.line1(L"Checking the files found...");
    return st.verify_candidates(&pr);
}

// First the paths that were verified last time (instant when unchanged), then the usual places.
bool quick_search(State& st) {
    {
        Progress pr(L"Checking the files found last time...", st.ui);
        for (size_t i = 0; i < kCount; i++) {
            if (st.prevPath[i].empty() || !exists(st.prevPath[i])) continue;
            bool cancelled = false;
            st.try_file(i, st.prevPath[i], &pr, &cancelled);
            if (cancelled) return false;
        }
    }
    return search(st, usual_places(st), L"Looking for your game files...");
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// DLC: unpacking a verified package into game\dlc\<content id>\ with <content id>.name beside it (its display name), which is how the host
// lists DLC (xam.cpp list_content). The STFS reader is BCR2's (C:\dev\bcr2\host\extract.cpp), with wide paths.
// ------------------------------------------------------------------------------------------------------------------------------------------------
bool is_dlc(size_t i) { return kSetupFiles[i].stfsType == 2; }
std::wstring dlc_dir(const State& st, size_t i) { return st.root + L"\\game\\dlc\\" + kSetupFiles[i].usualName; }
bool dlc_installed(const State& st, size_t i) { return exists(dlc_dir(st, i)) && exists(dlc_dir(st, i) + L".name"); }

uint32_t le24(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }

struct Stfs {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t base = 0;
    int shift = 0;   // 0: hash tables are one block, 1: two
    ~Stfs() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool readAt(uint64_t off, void* dst, size_t n) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        uint8_t* d = (uint8_t*)dst;
        while (n) { DWORD got = 0; if (!ReadFile(h, d, (DWORD)std::min<size_t>(n, 1u << 28), &got, nullptr) || !got) return false; d += got; n -= got; }
        return true;
    }
    // Data block n sits after the hash tables before it: one level-0 table per 170 blocks, one level-1 per 170^2, one level-2 per 170^3.
    uint64_t backing(uint32_t n) const {
        uint64_t b = n + (uint64_t)((n + 0xAA) / 0xAA << shift);
        if (n >= 0xAA) b += (uint64_t)((n + 0x70E4) / 0x70E4 << shift);
        if (n >= 0x70E4) b += (uint64_t)((n + 0x4AF768) / 0x4AF768 << shift);
        return b;
    }
    uint64_t offset(uint32_t n) const { return base + backing(n) * 0x1000; }
    bool nextBlock(uint32_t n, uint32_t& next) {   // from the level-0 hash table in front of the block's group
        uint8_t e[24];
        if (!readAt(offset(n / 0xAA * 0xAA) - 0x1000 + (uint64_t)(n % 0xAA) * 24, e, 24)) return false;
        next = be24(e + 0x15);
        return true;
    }
};

void remove_tree(const std::wstring& dir) {
    if (!exists(dir)) return;
    std::wstring from = dir; from.push_back(L'\0');   // SHFileOperation wants a double-terminated list
    SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

void make_dirs(const std::wstring& d) {
    for (size_t k = 0; (k = d.find(L'\\', k + 1)) != std::wstring::npos;) CreateDirectoryW(d.substr(0, k).c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
}

// Every file of an STFS package (DLC, title update) into the folder out (made empty first). displayOut: the package's display name.
bool extract_stfs(const std::wstring& pkg, const std::wstring& out, Progress* pr, std::wstring& err, std::wstring* displayOut = nullptr) {
    const std::wstring& tmp = out;
    Stfs s;
    s.h = CreateFileW(pkg.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s.h == INVALID_HANDLE_VALUE) { err = L"cannot open " + pkg; return false; }
    uint8_t hdr[0x1000];
    if (!s.readAt(0, hdr, sizeof hdr)) { err = L"cannot read the package header"; return false; }
    s.base = ((uint64_t)be32(hdr + 0x340) + 0xFFF) & 0xF000;
    const uint8_t* vd = hdr + 0x379;
    if (vd[0] != 0x24) { err = L"unexpected volume descriptor"; return false; }
    s.shift = (vd[2] & 1) ? 0 : 1;
    const uint32_t ftBlocks = vd[3] | vd[4] << 8, ftStart = le24(vd + 5);
    if (!ftBlocks || ftBlocks > 4096) { err = L"the package's file table looks damaged"; return false; }
    std::wstring display;
    for (int k = 0; k < 0x80; k++) { const wchar_t c = (wchar_t)(hdr[0x411 + 2 * k] << 8 | hdr[0x412 + 2 * k]); if (!c) break; display += c; }

    std::vector<uint8_t> ft((size_t)ftBlocks * 0x1000);
    for (uint32_t b = 0; b < ftBlocks; b++) if (!s.readAt(s.offset(ftStart + b), &ft[(size_t)b * 0x1000], 0x1000)) { err = L"cannot read the file table"; return false; }
    struct Entry { std::wstring name; bool dir, consecutive; uint32_t start, size; uint16_t parent; };
    std::vector<Entry> es;
    for (size_t o = 0; o + 0x40 <= ft.size(); o += 0x40) {
        const uint8_t* e = &ft[o];
        const int nl = e[0x28] & 0x3F;
        if (!nl) break;
        Entry en{ std::wstring(e, e + nl), (e[0x28] & 0x80) != 0, (e[0x28] & 0x40) != 0, le24(e + 0x2F), be32(e + 0x34), (uint16_t)(e[0x32] << 8 | e[0x33]) };
        es.push_back(en);
    }
    if (es.empty()) { err = L"the package contains no files"; return false; }
    auto pathOf = [&](size_t k) {
        std::wstring p = es[k].name; int guard = 0;
        for (uint16_t par = es[k].parent; par != 0xFFFF && par < es.size() && guard < 64; par = es[par].parent, guard++) p = es[par].name + L"\\" + p;
        return p;
    };
    uint64_t total = 0, done = 0; for (auto& e : es) if (!e.dir) total += e.size;
    remove_tree(tmp);
    make_dirs(tmp);
    if (!exists(tmp)) { err = L"cannot create " + tmp; return false; }
    std::vector<uint8_t> buf(0x1000 * 64);
    for (size_t k = 0; k < es.size(); k++) {
        const Entry& e = es[k];
        const std::wstring rel = pathOf(k), full = tmp + L"\\" + rel;
        if (e.dir) { make_dirs(full); continue; }
        make_dirs(parent(full));
        HANDLE out = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { err = L"cannot create " + full; remove_tree(tmp); return false; }
        uint64_t left = e.size; uint32_t blk = e.start; bool ok = true;
        while (left && ok) {
            size_t run = 1;   // a run of blocks stored next to each other
            if (e.consecutive) { const uint64_t want = (left + 0xFFF) / 0x1000; while (run < buf.size() / 0x1000 && run < want && s.backing(blk + (uint32_t)run) == s.backing(blk) + run) run++; }
            const size_t n = (size_t)std::min<uint64_t>(left, run * 0x1000ull);
            DWORD w = 0;
            if (!s.readAt(s.offset(blk), buf.data(), n)) { err = L"the package ends early (" + rel + L")"; ok = false; break; }
            if (!WriteFile(out, buf.data(), (DWORD)n, &w, nullptr) || w != n) { err = L"cannot write " + full + L" (disk full?)"; ok = false; break; }
            left -= n; done += n;
            if (left) {
                if (e.consecutive) blk += (uint32_t)run;
                else if (!s.nextBlock(blk, blk)) { err = L"cannot follow the block chain of " + rel; ok = false; }
            }
            if (pr && !pr->update(done, total, rel)) { err = L"cancelled"; ok = false; }
        }
        CloseHandle(out);
        if (!ok) { remove_tree(tmp); return false; }
    }
    if (displayOut) *displayOut = display;
    return true;
}

// Unpacks the verified DLC package of entry i. Written to <dir>.tmp and moved into place at the end, so an interrupted run leaves nothing half done.
bool install_dlc(State& st, size_t i, Progress* pr, std::wstring& err) {
    const std::wstring dir = dlc_dir(st, i), tmp = dir + L".tmp";
    if (pr) pr->line1(std::wstring(L"Unpacking ") + kSetupFiles[i].title + L"...");
    std::wstring display;
    if (!extract_stfs(st.found[i].path, tmp, pr, err, &display)) return false;
    remove_tree(dir);
    if (!MoveFileExW(tmp.c_str(), dir.c_str(), 0)) { err = L"cannot move the unpacked files into place"; remove_tree(tmp); return false; }
    if (FILE* f = _wfopen((dir + L".name").c_str(), L"wb")) { const std::string n = utf8(display.empty() ? kSetupFiles[i].title : display); fwrite(n.data(), 1, n.size(), f); fclose(f); }
    return true;
}

// Unpacks every verified DLC that isn't in game\dlc yet. Returns a line per package handled.
std::wstring install_verified_dlc(State& st) {
    std::wstring report;
    for (size_t i = 0; i < kCount; i++) {
        if (!is_dlc(i) || st.found[i].status != Status::Verified || dlc_installed(st, i)) continue;
        std::wstring err; bool ok;
        { Progress pr(L"Unpacking DLC...", st.ui); ok = install_dlc(st, i, &pr, err); }
        report += std::wstring(L"  - ") + kSetupFiles[i].title + (ok ? L": unpacked into game\\dlc" : L": NOT unpacked (" + err + L")") + L"\n";
        if (!st.ui) fprintf(stderr, "%s\n", utf8(report.substr(report.rfind(L"  - "))).c_str());
    }
    return report;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Unpacking the game: what the host loads (host\main.cpp, kfile.cpp)
//   game\disc\                   the disc's files, without $SystemUpdate\ and default.xex (tools\xdvdfs.py did the same)
//   game\update\                 the UPDATE: drive (empty: the title update holds no data files)
//   private\default.xex          from the disc
//   private\default.xexp         from the title update package
//   private\default_patched.xex  the update applied, decrypted and decompressed (XenonRecomp's XexPatcher, as XenonRecomp made it)
//   private\image.bin            the flat memory image at 0x82000000 (as tools\xexinfo.py makes it)
// Each step is skipped when its output is already there and right, and every output is checked against the hash of the file the recompiled
// code was made from.
// ------------------------------------------------------------------------------------------------------------------------------------------------
struct Derived { const wchar_t* rel; uint64_t size; const char* sha256; };
const Derived kDerived[] = {
    { L"private\\default.xex", 19644416ull, "c7e324a807a4407239b8fbfabe4377c0623889daa772eadac42c8b8f86782479" },
    { L"private\\default.xexp", 573440ull, "918cdee90389f27dee0bfe1bdfd758bc52cc4148666dcae1e35a1c0297f2005c" },
    { L"private\\default_patched.xex", 20529152ull, "5c00e0580849cc46119e7003a7caede861b663c70f767f5188e84fd4b31ffbaa" },
    { L"private\\image.bin", 20512768ull, "0d97444aff937e52164c45dc23cb561fae9ee60e958f61f47a92d4c0df0b1759" },
};
enum { kDefaultXex, kXexp, kPatched, kImage };

bool derived_ok(State& st, int k) {
    const std::wstring p = st.root + L"\\" + kDerived[k].rel;
    uint64_t size = 0, time = 0;
    return file_stat(p, size, time) && size == kDerived[k].size && st.sha_of(p, nullptr, nullptr) == kDerived[k].sha256;
}

bool read_at(HANDLE h, uint64_t off, void* dst, size_t n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    uint8_t* d = (uint8_t*)dst;
    while (n) { DWORD got = 0; if (!ReadFile(h, d, (DWORD)std::min<size_t>(n, 1u << 26), &got, nullptr) || !got) return false; d += got; n -= got; }
    return true;
}
bool write_file(const std::wstring& path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true; const uint8_t* p = (const uint8_t*)data;
    while (n && ok) { DWORD w = 0; const DWORD k = (DWORD)std::min<size_t>(n, 1u << 26); ok = WriteFile(h, p, k, &w, nullptr) && w == k; p += k; n -= k; }
    CloseHandle(h);
    return ok;
}
bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    const bool ok = read_at(h, 0, out.data(), out.size());
    CloseHandle(h);
    return ok;
}
// Writes under a temporary name and renames, so a reader never sees half a file.
bool replace_file(const std::wstring& path, const void* data, size_t n) {
    const std::wstring tmp = path + L".tmp";
    if (!write_file(tmp, data, n)) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// XDVDFS (the Xbox 360 disc file system), as tools\xdvdfs.py reads it.
struct IsoEntry { std::wstring path; uint32_t sector = 0, size = 0; bool dir = false; };
bool iso_walk(HANDLE h, uint64_t base, uint32_t sector, uint32_t size, const std::wstring& prefix, std::vector<IsoEntry>& out, int depth) {
    if (!size) return true;
    if (depth > 32 || size > (64u << 20)) return false;
    std::vector<uint8_t> d(size);
    if (!read_at(h, base + (uint64_t)sector * 0x800, d.data(), size)) return false;
    std::vector<uint32_t> stack{ 0 }; std::set<uint32_t> seen;
    while (!stack.empty()) {
        const uint32_t off = stack.back(); stack.pop_back();
        if (seen.count(off) || off + 14 > d.size()) continue;
        seen.insert(off);
        const uint16_t left = d[off] | d[off + 1] << 8, right = d[off + 2] | d[off + 3] << 8;
        if (left == 0xFFFF) continue;
        IsoEntry e;
        e.sector = d[off + 4] | d[off + 5] << 8 | d[off + 6] << 16 | (uint32_t)d[off + 7] << 24;
        e.size = d[off + 8] | d[off + 9] << 8 | d[off + 10] << 16 | (uint32_t)d[off + 11] << 24;
        const uint8_t attr = d[off + 12], nlen = d[off + 13];
        if (off + 14 + nlen > d.size()) continue;
        e.path = prefix + std::wstring(d.begin() + off + 14, d.begin() + off + 14 + nlen);
        e.dir = (attr & 0x10) != 0;
        out.push_back(e);
        if (e.dir && !iso_walk(h, base, e.sector, e.size, e.path + L"\\", out, depth + 1)) return false;
        if (left) stack.push_back(left * 4u);
        if (right) stack.push_back(right * 4u);
    }
    return true;
}
bool iso_entries(HANDLE h, uint64_t& base, std::vector<IsoEntry>& out) {
    static const uint64_t kPartitions[] = { 0, 0x2080000, 0xFD90000, 0x18300000 };   // plain, XGD3, XGD2, XGD1
    for (uint64_t b : kPartitions) {
        uint8_t vd[28];
        if (!read_at(h, b + 32 * 0x800, vd, sizeof vd) || memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) continue;
        base = b;
        const uint32_t rootSec = vd[20] | vd[21] << 8 | vd[22] << 16 | (uint32_t)vd[23] << 24, rootSize = vd[24] | vd[25] << 8 | vd[26] << 16 | (uint32_t)vd[27] << 24;
        return iso_walk(h, b, rootSec, rootSize, L"", out, 0);
    }
    return false;
}
// The disc files the game reads: everything but the dashboard update and the executable (which goes to private\).
bool disc_wanted(const IsoEntry& e) {
    const std::wstring l = lower(e.path);
    return l.rfind(L"$systemupdate", 0) != 0 && l != L"default.xex";
}
bool disc_up_to_date(const State& st, const std::vector<IsoEntry>& es) {
    for (const auto& e : es) {
        if (e.dir || !disc_wanted(e)) continue;
        uint64_t size = 0, time = 0;
        if (!file_stat(st.root + L"\\game\\disc\\" + e.path, size, time) || size != e.size) return false;
    }
    return true;
}

// Copies size bytes at off in h to a new file.
bool copy_out(HANDLE h, uint64_t off, uint64_t size, const std::wstring& path, std::vector<uint8_t>& buf, Progress* pr, uint64_t& done, uint64_t total, std::wstring& err) {
    HANDLE o = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (o == INVALID_HANDLE_VALUE) { err = L"cannot create " + path; return false; }
    bool ok = true;
    for (uint64_t left = size; left && ok;) {
        const size_t n = (size_t)std::min<uint64_t>(left, buf.size());
        DWORD w = 0;
        if (!read_at(h, off, buf.data(), n)) { err = L"the disc image ends early"; ok = false; break; }
        if (!WriteFile(o, buf.data(), (DWORD)n, &w, nullptr) || w != n) { err = L"cannot write " + path + L" (disk full?)"; ok = false; break; }
        off += n; left -= n; done += n;
        if (pr && !pr->update(done, total, path)) { err = L"cancelled"; ok = false; }
    }
    CloseHandle(o);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// The flat memory image of an unencrypted XEX (the patched one): tools\xexinfo.py.
bool xex_image(const std::vector<uint8_t>& x, std::vector<uint8_t>& img, std::wstring& err) {
    if (x.size() < 0x200 || memcmp(x.data(), "XEX2", 4)) { err = L"not an XEX2 file"; return false; }
    const uint32_t peOff = be32(&x[8]), secOff = be32(&x[16]), nhdr = be32(&x[20]);
    if (secOff + 0x114 > x.size() || 0x18 + 8ull * nhdr > x.size() || peOff > x.size()) { err = L"damaged XEX header"; return false; }
    const uint32_t imageSize = be32(&x[secOff + 4]);
    uint32_t ffi = 0;
    for (uint32_t k = 0; k < nhdr; k++) if (be32(&x[0x18 + 8 * k]) == 0x000003FF) ffi = be32(&x[0x18 + 8 * k + 4]);
    if (!ffi || ffi + 8 > x.size()) { err = L"no file format header"; return false; }
    const uint16_t enc = (uint16_t)(x[ffi + 4] << 8 | x[ffi + 5]), comp = (uint16_t)(x[ffi + 6] << 8 | x[ffi + 7]);
    if (enc != 0 || comp > 1) { err = L"the patched XEX is still encrypted or compressed"; return false; }
    img.assign(imageSize, 0);
    if (comp == 1) {   // basic: (data, zero) block pairs
        const uint32_t infoSize = be32(&x[ffi]);
        size_t src = peOff, dst = 0;
        for (uint32_t k = 0; k < (infoSize - 8) / 8; k++) {
            const uint32_t ds = be32(&x[ffi + 8 + 8 * k]), zs = be32(&x[ffi + 12 + 8 * k]);
            if (src + ds > x.size()) { err = L"damaged XEX data"; return false; }
            const size_t n = std::min<size_t>(ds, dst < imageSize ? imageSize - dst : 0);
            memcpy(img.data() + dst, x.data() + src, n);
            src += ds; dst += (size_t)ds + zs;
        }
    } else memcpy(img.data(), x.data() + peOff, std::min<size_t>(imageSize, x.size() - peOff));
    return true;
}

// Unpacks the verified disc image and title update. log gets a line per step.
bool unpack_game(State& st, std::wstring& log, std::wstring& err) {
    const std::wstring priv = st.root + L"\\private", game = st.root + L"\\game";
    make_dirs(priv); make_dirs(game + L"\\update");
    Progress pr(L"Unpacking the game...", st.ui);
    HANDLE iso = CreateFileW(st.found[0].path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (iso == INVALID_HANDLE_VALUE) { err = L"cannot open " + st.found[0].path; return false; }
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{ iso };
    uint64_t base = 0; std::vector<IsoEntry> es;
    if (!iso_entries(iso, base, es)) { err = L"cannot read the disc image's file system"; return false; }
    std::vector<uint8_t> buf(1 << 24);

    // 1. default.xex
    if (derived_ok(st, kDefaultXex)) log += L"  - private\\default.xex: already there\n";
    else {
        auto it = std::find_if(es.begin(), es.end(), [](const IsoEntry& e) { return lower(e.path) == L"default.xex"; });
        if (it == es.end()) { err = L"the disc image has no default.xex"; return false; }
        uint64_t done = 0; const std::wstring p = priv + L"\\default.xex";
        if (!copy_out(iso, base + (uint64_t)it->sector * 0x800, it->size, p + L".tmp", buf, &pr, done, it->size, err)) return false;
        if (!MoveFileExW((p + L".tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) { err = L"cannot write " + p; return false; }
        st.cache.erase(lower(p));
        if (!derived_ok(st, kDefaultXex)) { err = L"private\\default.xex is not the executable this build was made from"; return false; }
        log += L"  - private\\default.xex: written\n";
    }
    // 2. the disc's files
    if (disc_up_to_date(st, es)) log += L"  - game\\disc: already there\n";
    else {
        const std::wstring tmp = game + L"\\disc.tmp", dir = game + L"\\disc";
        remove_tree(tmp); make_dirs(tmp);
        uint64_t total = 0, done = 0; size_t files = 0;
        for (const auto& e : es) if (!e.dir && disc_wanted(e)) total += e.size;
        pr.line1(L"Unpacking the disc's files (about " + gb(total) + L")...");
        for (const auto& e : es) {
            if (!disc_wanted(e)) continue;
            const std::wstring out = tmp + L"\\" + e.path;
            if (e.dir) { make_dirs(out); continue; }
            make_dirs(parent(out));
            if (!copy_out(iso, base + (uint64_t)e.sector * 0x800, e.size, out, buf, &pr, done, total, err)) { remove_tree(tmp); return false; }
            files++;
        }
        remove_tree(dir);
        if (!MoveFileExW(tmp.c_str(), dir.c_str(), 0)) { err = L"cannot move the disc's files into place"; remove_tree(tmp); return false; }
        log += L"  - game\\disc: " + std::to_wstring(files) + L" files, " + gb(total) + L"\n";
    }
    // 3. default.xexp from the title update
    if (derived_ok(st, kXexp)) log += L"  - private\\default.xexp: already there\n";
    else {
        const std::wstring tmp = priv + L"\\tu.tmp", p = priv + L"\\default.xexp";
        pr.line1(L"Unpacking the title update...");
        if (!extract_stfs(st.found[1].path, tmp, &pr, err)) return false;
        const bool moved = MoveFileExW((tmp + L"\\default.xexp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        remove_tree(tmp);
        if (!moved) { err = L"the title update package has no default.xexp"; return false; }
        st.cache.erase(lower(p));
        if (!derived_ok(st, kXexp)) { err = L"private\\default.xexp is not the update this build was made from"; return false; }
        log += L"  - private\\default.xexp: written\n";
    }
    // 4. the update applied
    std::vector<uint8_t> patched;
    const std::wstring patchedPath = priv + L"\\default_patched.xex";
    if (derived_ok(st, kPatched)) { log += L"  - private\\default_patched.xex: already there\n"; if (!derived_ok(st, kImage) && !read_file(patchedPath, patched)) { err = L"cannot read " + patchedPath; return false; } }
    else {
        pr.line1(L"Applying the title update...");
        std::vector<uint8_t> xex, xexp;
        if (!read_file(priv + L"\\default.xex", xex) || !read_file(priv + L"\\default.xexp", xexp)) { err = L"cannot read private\\default.xex / default.xexp"; return false; }
        const XexPatcher::Result r = XexPatcher::apply(xex.data(), xex.size(), xexp.data(), xexp.size(), patched, false);
        if (r != XexPatcher::Result::Success) { err = L"applying the title update failed (XexPatcher result " + std::to_wstring((int)r) + L")"; return false; }
        if (!replace_file(patchedPath, patched.data(), patched.size())) { err = L"cannot write " + patchedPath; return false; }
        st.cache.erase(lower(patchedPath));
        if (!derived_ok(st, kPatched)) { err = L"private\\default_patched.xex does not match the one this build was made from"; return false; }
        log += L"  - private\\default_patched.xex: written\n";
    }
    // 5. the memory image
    if (derived_ok(st, kImage)) log += L"  - private\\image.bin: already there\n";
    else {
        std::vector<uint8_t> img;
        if (!xex_image(patched, img, err)) return false;
        const std::wstring p = priv + L"\\image.bin";
        if (!replace_file(p, img.data(), img.size())) { err = L"cannot write " + p; return false; }
        st.cache.erase(lower(p));
        if (!derived_ok(st, kImage)) { err = L"private\\image.bin does not match the one this build was made from"; return false; }
        log += L"  - private\\image.bin: written\n";
    }
    st.save();
    return true;
}

// Whether everything unpack_game makes is in place (reads the disc image's directory and hashes the four private files, through the cache).
bool game_unpacked(State& st) {
    for (int k = 0; k < 4; k++) if (!derived_ok(st, k)) return false;
    if (st.found[0].status != Status::Verified) return exists(st.root + L"\\game\\disc\\BCGame");
    HANDLE iso = CreateFileW(st.found[0].path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (iso == INVALID_HANDLE_VALUE) return false;
    uint64_t base = 0; std::vector<IsoEntry> es;
    const bool ok = iso_entries(iso, base, es) && disc_up_to_date(st, es);
    CloseHandle(iso);
    return ok;
}

// Which known file this is, checked by size and SHA-256. -1 when none: why says what it is instead.
int identify(State& st, const std::wstring& path, Progress* pr, std::wstring& why) {
    uint64_t size = 0, time = 0;
    if (!file_stat(path, size, time)) { why = L"cannot be read"; return -1; }
    for (size_t i = 0; i < kCount; i++) {
        if (kSetupFiles[i].size != size) continue;
        bool cancelled = false;
        if (st.try_file(i, path, pr, &cancelled)) return (int)i;
        why = cancelled ? L"not checked (cancelled)" : std::wstring(L"the size of ") + kSetupFiles[i].title + L" but a different SHA-256 (another version or a damaged copy)";
        return -1;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    uint8_t hdr[0x400] = {}; DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, hdr, sizeof hdr, &got, nullptr); CloseHandle(h); }
    if (got == sizeof hdr && (!memcmp(hdr, "LIVE", 4) || !memcmp(hdr, "PIRS", 4) || !memcmp(hdr, "CON ", 4)))
        why = be32(hdr + 0x360) == kTitleId ? L"an Asura's Wrath package this recomp doesn't know (another version?)" : L"a content package for another game";
    else why = L"not an Xbox 360 content package";
    return -1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Text for the dialogs and the report
// ------------------------------------------------------------------------------------------------------------------------------------------------
const wchar_t* need_text(Need n) { return n == Need::Required ? L"required" : n == Need::Recommended ? L"strongly recommended" : L"optional"; }

std::wstring status_line(const State& st, size_t i) {
    const KnownFile& k = kSetupFiles[i];
    const Found& x = st.found[i];
    std::wstring s = std::wstring(k.title) + L" (" + need_text(k.need) + L"): ";
    if (x.status == Status::Verified) return s + (is_dlc(i) ? (dlc_installed(st, i) ? L"verified, installed" : L"verified, not unpacked yet") : L"verified") + L"\n      " + x.path;
    if (x.status == Status::Skipped) s += L"skipped";
    else s += L"not found";
    for (const auto& m : x.mismatches)
        s += L"\n      different version: " + m.path + L" (" + (m.sha.empty() ? gb(m.size) + L", needs " + gb(k.size) : L"SHA-256 " + wide(m.sha.substr(0, 16)) + L"...") + L")";
    return s;
}

std::wstring summary(const State& st, bool onlyMissing) {
    std::wstring s;
    for (size_t i = 0; i < kCount; i++) {
        if (onlyMissing && st.found[i].status == Status::Verified) continue;
        s += L"  - " + status_line(st, i) + L"\n";
    }
    return s;
}

std::wstring needed_text(size_t i) {
    const KnownFile& k = kSetupFiles[i];
    return std::wstring(L"  Usual name: ") + k.usualName + L"\n  Size: " + std::to_wstring(k.size) + L" bytes (" + gb(k.size) + L")\n  SHA-256: " + wide(k.sha256);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Dialogs
// ------------------------------------------------------------------------------------------------------------------------------------------------
int box(const std::wstring& text, UINT flags) { return MessageBoxW(nullptr, text.c_str(), kTitle, flags | MB_SETFOREGROUND); }

bool browse_file(size_t i, std::wstring& path, const wchar_t* extra) {
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring title = std::wstring(L"Select ") + kSetupFiles[i].title + extra;
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o);
    o.lpstrFilter = kSetupFiles[i].stfsType == 0 ? L"Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0" : L"All files (*.*)\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH * 4;
    o.lpstrTitle = title.c_str();
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&o)) return false;
    path = file; return true;
}

bool browse_folder(std::wstring& dir) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0; d->GetOptions(&opts); d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Choose a folder to search for your Asura's Wrath files");
    bool ok = false;
    if (SUCCEEDED(d->Show(nullptr))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { dir = p; CoTaskMemFree(p); ok = true; }
            it->Release();
        }
    }
    d->Release();
    return ok;
}

std::wstring dlc_status(const State& st) {
    std::wstring s;
    for (size_t i = 0; i < kCount; i++) {
        if (!is_dlc(i)) continue;
        s += std::wstring(L"  - ") + kSetupFiles[i].title + (kSetupFiles[i].need == Need::Recommended ? L" (strongly recommended)" : L"") + L": " +
             (dlc_installed(st, i) ? L"installed" : st.found[i].status == Status::Verified ? L"verified, not unpacked yet" : L"not added") + L"\n";
    }
    return s;
}

// --unpack: find and verify the files (as --check), then unpack the game and any verified DLC, without dialogs.
int run_unpack(State& st) {
    quick_search(st);
    st.save();
    fputs(utf8(summary(st, false)).c_str(), stdout);
    if (!st.allRequired()) { puts("The disc image and the title update must both be verified first."); return 1; }
    std::wstring log, err;
    const DWORD t0 = GetTickCount();
    const bool ok = unpack_game(st, log, err);
    fputs(utf8(log).c_str(), stdout);
    if (!ok) { printf("unpacking failed: %s\n", utf8(err).c_str()); return 1; }
    const std::wstring dlc = install_verified_dlc(st);
    st.save();
    printf("%sdone in %.0f s\n", utf8(dlc).c_str(), (GetTickCount() - t0) / 1000.0);
    return 0;
}

// --add <file>...: the same without dialogs.
int run_add_files(State& st, const std::vector<std::wstring>& files) {
    for (size_t i = 0; i < kCount; i++) if (!st.prevPath[i].empty() && exists(st.prevPath[i])) { bool c = false; st.try_file(i, st.prevPath[i], nullptr, &c); }
    int bad = 0;
    for (const auto& f : files) {
        std::wstring why;
        const int i = identify(st, f, nullptr, why);
        if (i < 0) { bad++; printf("%s: %s\n", utf8(f).c_str(), utf8(why).c_str()); }
        else printf("%s: %s, verified\n", utf8(f).c_str(), utf8(kSetupFiles[i].title).c_str());
    }
    const std::wstring rep = install_verified_dlc(st);
    st.save();
    printf("%sDLC:\n%s", utf8(rep).c_str(), utf8(dlc_status(st)).c_str());
    fflush(stdout);
    return bad ? 1 : 0;
}

int run_check(State& st, const std::vector<std::wstring>& folders, bool drives, const std::wstring& report) {
    bool ok = quick_search(st);
    if (ok && !folders.empty()) {
        std::vector<Place> v; for (auto& f : folders) v.push_back({ f, 12 });
        ok = search(st, v, L"");
    }
    if (ok && drives && !st.allFound()) ok = search(st, all_drives(), L"");
    st.save();
    std::string out = "Asura's Wrath recomp setup check (data folder " + utf8(st.root) + ")\n" + utf8(summary(st, false)) +
                      (st.allRequired() ? "Every required file is verified.\n" : "Required files are missing.\n") + "Saved: " + utf8(st.sourcesPath) + "\n";
    fputs(out.c_str(), stdout); fflush(stdout);
    if (!report.empty()) if (FILE* f = _wfopen(report.c_str(), L"wb")) { fputs(out.c_str(), f); fclose(f); }
    return st.allRequired() ? 0 : 1;
}


// ------------------------------------------------------------------------------------------------------------------------------------------------
// The setup window: a row per file (path box, Browse..., a red/green status), drag and drop, OK once the disc image and the title update are
// verified or already installed. Searching, checking and unpacking run on a worker thread on a copy of the state; results come back as
// messages and are merged on the window's thread, which alone owns the real State.
// ------------------------------------------------------------------------------------------------------------------------------------------------
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

enum class RowState { Searching, Empty, Checking, Wrong, Verified, Installed };
struct Row { HWND label = nullptr, edit = nullptr, browse = nullptr, status = nullptr; RowState state = RowState::Searching; std::wstring path; int pct = -1; };

struct Job { enum Kind { Startup, Check, Search, Install } kind = Startup; int row = -1; std::wstring path; State snap; };
struct Result { Job::Kind kind = Job::Startup; int row = -1; std::wstring path, why, log; bool ok = false, cancelled = false, gameInstalled = false; State snap; };

constexpr UINT WM_APP_RESULT = WM_APP + 1, WM_APP_PROGRESS = WM_APP + 2;
enum { ID_EDIT = 100, ID_BROWSE = 200, ID_OK = 1, ID_CANCEL = 2, ID_SEARCH = 300 };

struct Ui {
    HWND wnd = nullptr, header = nullptr, statusText = nullptr, progress = nullptr, searchBtn = nullptr, okBtn = nullptr, cancelBtn = nullptr;
    Row rows[kCount];
    HFONT font = nullptr, bold = nullptr;
    int dpi = 96;
    State* st = nullptr;
    bool gameInstalled = false, closing = false;
    int pending = 0;            // jobs queued or running
    bool installing = false;
    std::wstring status;
} g;

std::mutex g_jobLock; std::condition_variable g_jobCv; std::deque<Job> g_jobs; std::atomic<bool> g_cancel{ false };
std::mutex g_progLock; std::wstring g_progLine1, g_progLine2; uint64_t g_progDone = 0, g_progTotal = 0; std::atomic<int> g_progRow{ -1 };

int px(int v) { return MulDiv(v, g.dpi, 96); }

void enqueue(Job j) {
    g.pending++;
    { std::lock_guard<std::mutex> lk(g_jobLock); g_jobs.push_back(std::move(j)); }
    g_jobCv.notify_one();
}

void worker() {
    g_progressHook = [](uint64_t done, uint64_t total, const std::wstring& line2, const std::wstring* line1) {
        if (done != UINT64_MAX) {
            std::lock_guard<std::mutex> lk(g_progLock);
            if (line1) { g_progLine1 = *line1; g_progLine2.clear(); g_progDone = 0; g_progTotal = 0; }
            else { g_progDone = done; g_progTotal = total; g_progLine2 = line2; }
            PostMessageW(g.wnd, WM_APP_PROGRESS, 0, 0);
        }
        return !g_cancel.load();
    };
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_jobLock);
            g_jobCv.wait(lk, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front()); g_jobs.pop_front();
        }
        g_cancel = false;
        g_progRow = j.kind == Job::Check ? j.row : -1;
        auto* r = new Result;
        r->kind = j.kind; r->row = j.row; r->path = j.path;
        State& s = j.snap; s.ui = false;
        switch (j.kind) {
        case Job::Startup:
            r->cancelled = !quick_search(s);
            r->gameInstalled = game_unpacked(s);
            break;
        case Job::Search:
            r->cancelled = !search(s, { { j.path, 12 } }, (L"Searching " + j.path + L"...").c_str());
            break;
        case Job::Check: {
            uint64_t size = 0, time = 0;
            const size_t i = (size_t)j.row;
            s.found[i] = Found();
            if (!file_stat(j.path, size, time)) { r->why = L"cannot be read"; break; }
            if (size != kSetupFiles[i].size) { r->why = L"wrong size (" + std::to_wstring(size) + L" bytes; needs " + std::to_wstring(kSetupFiles[i].size) + L")"; break; }
            Progress pr((L"Checking " + file_name(j.path) + L" (" + gb(size) + L")...").c_str(), false);
            bool cancelled = false;
            r->ok = s.try_file(i, j.path, &pr, &cancelled);
            r->cancelled = cancelled;
            if (!r->ok && !cancelled) r->why = L"different SHA-256: another version or a damaged copy";
            break;
        }
        case Job::Install: {
            std::wstring log, err;
            bool ok = true;
            if (s.found[0].status == Status::Verified && s.found[1].status == Status::Verified && !game_unpacked(s)) ok = unpack_game(s, log, err);
            if (ok) log += install_verified_dlc(s);
            r->ok = ok; r->log = log; r->why = err; r->cancelled = g_cancel.load();
            r->gameInstalled = game_unpacked(s);
            break;
        }
        }
        r->snap = std::move(s);
        PostMessageW(g.wnd, WM_APP_RESULT, 0, (LPARAM)r);
    }
}

// The recomp itself: asura.exe beside setup, or in the data folder (host\build\asura.exe in the dev layout). "" if neither.
std::wstring game_exe() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    for (const std::wstring& c : { parent(p) + L"\\asura.exe", g.st->root + L"\\asura.exe", g.st->root + L"\\host\\build\\asura.exe" })
        if (exists(c)) return c;
    return L"";
}

bool row_ok(size_t i) { return g.rows[i].state == RowState::Verified || g.rows[i].state == RowState::Installed; }
bool row_installed(size_t i) { return is_dlc(i) ? dlc_installed(*g.st, i) : g.gameInstalled; }

void set_status(const std::wstring& t) { g.status = t; SetWindowTextW(g.statusText, t.c_str()); }

void refresh() {
    for (size_t i = 0; i < kCount; i++) {
        Row& r = g.rows[i];
        std::wstring t;
        switch (r.state) {
        case RowState::Searching: t = L"\x25CF  Searching..."; break;
        case RowState::Checking: t = r.pct >= 0 ? L"\x25CF  Checking... " + std::to_wstring(r.pct) + L"%" : L"\x25CF  Checking..."; break;
        case RowState::Empty: t = is_dlc(i) ? L"\x25CF  Not added" : L"\x25CF  Not found"; break;
        case RowState::Wrong: t = L"\x25CF  Wrong file"; break;
        case RowState::Verified: t = row_installed(i) ? L"\x25CF  Verified, installed" : L"\x25CF  Verified"; break;
        case RowState::Installed: t = L"\x25CF  Installed"; break;
        }
        SetWindowTextW(r.status, t.c_str());
        InvalidateRect(r.status, nullptr, TRUE);
        const bool idle = !g.installing;
        EnableWindow(r.edit, idle); EnableWindow(r.browse, idle);
    }
    EnableWindow(g.okBtn, !g.installing && g.pending == 0 && row_ok(0) && row_ok(1));
    EnableWindow(g.searchBtn, !g.installing);
    SetWindowTextW(g.cancelBtn, g.pending ? L"Stop" : L"Close");
}

// Row i without a verified file: installed, or red (with what was found instead).
void settle_row(size_t i) {
    Row& r = g.rows[i];
    if (r.state == RowState::Verified || r.state == RowState::Checking) return;
    if (!r.path.empty()) { r.state = RowState::Wrong; return; }
    r.state = row_installed(i) ? RowState::Installed : RowState::Empty;
}

void check_row(size_t i, const std::wstring& path) {
    Row& r = g.rows[i];
    r.path = path;
    SetWindowTextW(r.edit, path.c_str());
    g.st->found[i] = Found();
    if (path.empty()) { r.state = RowState::Searching; settle_row(i); refresh(); return; }
    r.state = RowState::Checking; r.pct = -1;
    Job j; j.kind = Job::Check; j.row = (int)i; j.path = path; j.snap = *g.st;
    enqueue(std::move(j));
    refresh();
}

void merge_cache(const State& from) { for (auto& kv : from.cache) g.st->cache[kv.first] = kv.second; }

void on_result(Result* r) {
    g.pending--;
    merge_cache(r->snap);
    switch (r->kind) {
    case Job::Startup:
    case Job::Search:
        if (r->kind == Job::Startup) g.gameInstalled = r->gameInstalled;
        for (size_t i = 0; i < kCount; i++) {
            Row& row = g.rows[i];
            const Found& f = r->snap.found[i];
            if (row.state == RowState::Verified || row.state == RowState::Checking) continue;   // the player's own choice wins
            if (f.status == Status::Verified) {
                g.st->found[i] = f; row.path = f.path; row.state = RowState::Verified; SetWindowTextW(row.edit, f.path.c_str());
            } else if (row.path.empty() && !f.mismatches.empty() && !row_installed(i)) {
                row.path = f.mismatches.front().path; SetWindowTextW(row.edit, row.path.c_str()); row.state = RowState::Wrong;
            } else { if (row.state == RowState::Searching) row.state = RowState::Empty; settle_row(i); }
        }
        set_status(r->cancelled ? L"Search stopped." : r->kind == Job::Startup ? L"Ready. Fill in anything missing: type a path, Browse..., or drop files on this window." : L"Search finished.");
        break;
    case Job::Check: {
        Row& row = g.rows[r->row];
        if (row.path != r->path || row.state != RowState::Checking) break;   // the box changed meanwhile: a newer check follows
        row.pct = -1;
        if (r->ok) { g.st->found[r->row] = r->snap.found[r->row]; row.state = RowState::Verified; set_status(std::wstring(kSetupFiles[r->row].title) + L": verified."); }
        else { row.state = RowState::Wrong; set_status(std::wstring(kSetupFiles[r->row].title) + L": " + (r->cancelled ? L"check stopped" : L"not the right file, " + r->why) + L"."); }
        break;
    }
    case Job::Install:
        g.installing = false;
        g.gameInstalled = r->gameInstalled;
        for (size_t i = 0; i < kCount; i++) g.st->found[i] = r->snap.found[i];
        g.st->save();
        for (size_t i = 0; i < kCount; i++) settle_row(i);
        SendMessageW(g.progress, PBM_SETPOS, 0, 0);
        refresh();
        if (r->cancelled) set_status(L"Stopped. Nothing half-done was left in place; press OK to continue.");
        else if (!r->ok) { set_status(L"Unpacking failed: " + r->why); MessageBoxW(g.wnd, (L"Unpacking failed:\n" + r->why).c_str(), kTitle, MB_OK | MB_ICONERROR); }
        else {
            set_status(L"Done.");
            const std::wstring done = L"Setup is complete.\n\n" + (r->log.empty() ? std::wstring(L"Everything was already in place.\n") : r->log) +
                                      L"\nRun setup again at any time to add DLC.";
            const std::wstring game = game_exe();
            if (!game.empty()) {
                if (MessageBoxW(g.wnd, (done + L"\n\nStart the game now?").c_str(), kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
                    ShellExecuteW(nullptr, L"open", game.c_str(), nullptr, parent(game).c_str(), SW_SHOWNORMAL);
            } else MessageBoxW(g.wnd, done.c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
            DestroyWindow(g.wnd);
        }
        delete r;
        return;
    }
    g.st->save();
    if (g.pending == 0) SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    refresh();
    delete r;
}

void on_progress() {
    std::wstring l1, l2; uint64_t done, total;
    { std::lock_guard<std::mutex> lk(g_progLock); l1 = g_progLine1; l2 = g_progLine2; done = g_progDone; total = g_progTotal; }
    const int pos = total ? (int)(done * 1000 / total) : 0;
    SendMessageW(g.progress, PBM_SETPOS, pos, 0);
    std::wstring t = l1;
    if (!l2.empty()) { std::wstring f = l2; if (f.size() > 70) f = L"..." + f.substr(f.size() - 67); t += L"  " + f; }
    if (t != g.status) set_status(t);
    const int row = g_progRow.load();
    if (row >= 0 && g.rows[row].state == RowState::Checking && total) {
        const int pct = (int)(done * 100 / total);
        if (pct != g.rows[row].pct) { g.rows[row].pct = pct; refresh(); }
    }
}

// Dropped files: one file on a row goes to that row; otherwise each goes to the row its size belongs to.
void on_drop(HDROP drop) {
    POINT pt; DragQueryPoint(drop, &pt);
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> files;
    for (UINT k = 0; k < n; k++) { wchar_t b[MAX_PATH * 4]; if (DragQueryFileW(drop, k, b, MAX_PATH * 4)) files.push_back(b); }
    DragFinish(drop);
    if (g.installing || files.empty()) return;
    if (files.size() == 1) {
        for (size_t i = 0; i < kCount; i++) {
            RECT rc; GetWindowRect(g.rows[i].label, &rc); RECT re; GetWindowRect(g.rows[i].status, &re);
            POINT a{ rc.left, rc.top }, b{ re.right, re.bottom }; ScreenToClient(g.wnd, &a); ScreenToClient(g.wnd, &b);
            if (pt.y >= a.y && pt.y <= b.y) { check_row(i, files[0]); return; }
        }
    }
    std::wstring unknown;
    for (const auto& f : files) {
        uint64_t size = 0, time = 0; bool placed = false;
        if (file_stat(f, size, time))
            for (size_t i = 0; i < kCount && !placed; i++) if (kSetupFiles[i].size == size) { check_row(i, f); placed = true; }
        if (!placed) unknown += (unknown.empty() ? L"" : L", ") + file_name(f);
    }
    if (!unknown.empty()) set_status(L"Not one of the files setup needs (by size): " + unknown);
}

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    return h;
}

void layout() {
    RECT rc; GetClientRect(g.wnd, &rc);
    const int W = rc.right, m = px(16), browseW = px(88), statusW = px(170), gap = px(8), rowH = px(58), ctrlH = px(25);
    MoveWindow(g.header, m, px(12), W - 2 * m, px(40), TRUE);
    int y = px(60);
    for (size_t i = 0; i < kCount; i++) {
        if (i == 2) y += px(10);   // a little space between the game and the DLC
        Row& r = g.rows[i];
        MoveWindow(r.label, m, y, W - 2 * m, px(20), TRUE);
        const int ey = y + px(22), editW = W - 2 * m - browseW - statusW - 2 * gap;
        MoveWindow(r.edit, m, ey, editW, ctrlH, TRUE);
        MoveWindow(r.browse, m + editW + gap, ey, browseW, ctrlH, TRUE);
        MoveWindow(r.status, m + editW + gap + browseW + gap, ey, statusW, ctrlH, TRUE);
        y += rowH;
    }
    y += px(6);
    MoveWindow(g.statusText, m, y, W - 2 * m, px(20), TRUE); y += px(24);
    MoveWindow(g.progress, m, y, W - 2 * m, px(14), TRUE);
    const int by = rc.bottom - m - px(28), bw = px(96);
    MoveWindow(g.searchBtn, m, by, px(150), px(28), TRUE);
    MoveWindow(g.cancelBtn, W - m - bw, by, bw, px(28), TRUE);
    MoveWindow(g.okBtn, W - m - 2 * bw - gap, by, bw, px(28), TRUE);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.wnd = h;
        NONCLIENTMETRICSW ncm{ sizeof(ncm) }; SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
        g.font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lb = ncm.lfMessageFont; lb.lfWeight = FW_SEMIBOLD; g.bold = CreateFontIndirectW(&lb);
        g.header = child(L"STATIC", L"Choose your own dumps of the Xbox 360 game. Each file is checked against the known SHA-256: green is ready, red is missing "
                                    L"or the wrong file. You can also drop files on this window. Run setup again later to add DLC.", SS_LEFT, -1);
        for (size_t i = 0; i < kCount; i++) {
            Row& r = g.rows[i];
            const KnownFile& k = kSetupFiles[i];
            const std::wstring label = std::wstring(k.title) + (k.need == Need::Required ? L"  (required)" : k.need == Need::Recommended ? L"  (DLC, strongly recommended: the story's real ending)" : L"  (DLC, optional)");
            r.label = child(L"STATIC", label.c_str(), SS_LEFT | SS_NOPREFIX, -1);
            SendMessageW(r.label, WM_SETFONT, (WPARAM)g.bold, TRUE);
            r.edit = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_EDIT + (int)i, WS_EX_CLIENTEDGE);
            SendMessageW(r.edit, EM_SETCUEBANNER, TRUE, (LPARAM)(std::wstring(L"Usual name: ") + k.usualName).c_str());
            r.browse = child(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, ID_BROWSE + (int)i);
            r.status = child(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, -1);
        }
        g.statusText = child(L"STATIC", L"Looking for your files...", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1);
        g.progress = child(PROGRESS_CLASSW, L"", 0, -1);
        SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
        g.searchBtn = child(L"BUTTON", L"Search a folder...", WS_TABSTOP | BS_PUSHBUTTON, ID_SEARCH);
        g.okBtn = child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_OK);
        g.cancelBtn = child(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, ID_CANCEL);
        DragAcceptFiles(h, TRUE);
        layout();
        refresh();
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = px(640); mm->ptMinTrackSize.y = px(600); return 0; }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; HWND c = (HWND)lp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        for (size_t i = 0; i < kCount; i++) if (c == g.rows[i].status) {
            const RowState s = g.rows[i].state;
            SetTextColor(dc, s == RowState::Verified || s == RowState::Installed ? RGB(0, 140, 60) : s == RowState::Empty || s == RowState::Wrong ? RGB(200, 30, 30) : RGB(120, 120, 120));
        }
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DROPFILES: on_drop((HDROP)wp); return 0;
    case WM_APP_RESULT: on_result((Result*)lp); return 0;
    case WM_APP_PROGRESS: on_progress(); return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_EDIT && id < ID_EDIT + (int)kCount && code == EN_KILLFOCUS) {
            const size_t i = id - ID_EDIT;
            wchar_t b[MAX_PATH * 4]; GetWindowTextW(g.rows[i].edit, b, MAX_PATH * 4);
            std::wstring t = b;
            while (!t.empty() && (t.back() == L' ' || t.back() == L'"')) t.pop_back();
            while (!t.empty() && (t.front() == L' ' || t.front() == L'"')) t.erase(0, 1);
            if (t != g.rows[i].path) check_row(i, t);
            return 0;
        }
        if (id >= ID_BROWSE && id < ID_BROWSE + (int)kCount) {
            std::wstring path;
            if (browse_file(id - ID_BROWSE, path, L"")) check_row(id - ID_BROWSE, path);
            return 0;
        }
        if (id == ID_SEARCH) {
            std::wstring dir;
            if (browse_folder(dir)) {
                for (size_t i = 0; i < kCount; i++) if (!row_ok(i) && g.rows[i].path.empty()) g.rows[i].state = RowState::Searching;
                Job j; j.kind = Job::Search; j.path = dir; j.snap = *g.st; enqueue(std::move(j)); refresh();
            }
            return 0;
        }
        if (id == ID_OK && IsWindowEnabled(g.okBtn)) {
            g.installing = true;
            set_status(L"Unpacking...");
            Job j; j.kind = Job::Install; j.snap = *g.st; enqueue(std::move(j));
            refresh();
            return 0;
        }
        if (id == ID_CANCEL) {
            if (g.pending) { g_cancel = true; set_status(L"Stopping..."); }
            else DestroyWindow(h);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (g.installing && MessageBoxW(h, L"Unpacking is still running. Stop it and close setup?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        g_cancel = true;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int run_window(State& st) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    g.st = &st;
    g.dpi = (int)GetDpiForSystem();
    WNDCLASSW wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AsuraSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{ 0, 0, px(780), px(600) }; AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
    HWND h = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) return 1;
    std::thread(worker).detach();
    input_guard_install(false);   // no Alt+Tab from Steam's controller chords while setup is in front; the controller may still move the mouse
    { Job j; j.kind = Job::Startup; j.snap = st; enqueue(std::move(j)); }
    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {   // Enter in a path box: check it now (by leaving the box)
            for (size_t i = 0; i < kCount; i++) if (m.hwnd == g.rows[i].edit) { SetFocus(g.rows[i].browse); break; }
        }
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // A windowed program: reach the console it was started from (if any) for --check and --hash output.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stdout);
        if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN) freopen("CONOUT$", "w", stderr);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    State st;
    st.root = data_root();
    st.sourcesPath = st.root + L"\\game\\sources.ini";

    bool check = false, drives = false, unpack = false; std::vector<std::wstring> folders, adds; std::wstring report;
    for (int a = 1; a < argc; a++) {
        std::wstring s = argv[a];
        if (s == L"--hash" && a + 1 < argc) {
            uint64_t size = 0, time = 0; std::wstring p = argv[a + 1];
            std::string sha = file_stat(p, size, time) ? file_sha256(p, nullptr) : "";
            printf("%s %llu %s\n", sha.empty() ? "unreadable" : sha.c_str(), (unsigned long long)size, utf8(p).c_str());
            return sha.empty() ? 1 : 0;
        }
        if (s == L"--check") check = true;
        else if (s == L"--unpack") unpack = true;
        else if (s == L"--add" && a + 1 < argc) adds.push_back(argv[++a]);
        else if (s == L"--drives") drives = true;
        else if (s == L"--search" && a + 1 < argc) folders.push_back(argv[++a]);
        else if (s == L"--report" && a + 1 < argc) report = argv[++a];
        else if (s == L"--root" && a + 1 < argc) { st.root = argv[++a]; st.sourcesPath = st.root + L"\\game\\sources.ini"; }
    }
    st.ui = !check && !unpack && adds.empty();
    st.load();
    int r = unpack ? run_unpack(st)
          : !adds.empty() ? run_add_files(st, adds)
          : check ? run_check(st, folders, drives, report)
          : run_window(st);
    CoUninitialize();
    return r;
}
