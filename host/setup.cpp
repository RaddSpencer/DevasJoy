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
struct Progress {
    IProgressDialog* d = nullptr;
    bool console = false;
    DWORD tick = 0;
    Progress(const wchar_t* line1, bool ui) {
        console = !ui;
        if (ui && SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
            d->SetTitle(kTitle);
            d->SetLine(1, line1, FALSE, nullptr);
            d->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
        }
    }
    ~Progress() { if (d) { d->StopProgressDialog(); d->Release(); } }
    void line1(const std::wstring& s) { if (d) d->SetLine(1, s.c_str(), FALSE, nullptr); }
    bool update(uint64_t done, uint64_t total, const std::wstring& line2) {
        if (GetTickCount() - tick < 100) return d ? !d->HasUserCancelled() : true;
        tick = GetTickCount();
        if (!d) return true;
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

// Unpacks the verified package of entry i. Written to <dir>.tmp and moved into place at the end, so an interrupted run leaves nothing half done.
bool install_dlc(State& st, size_t i, Progress* pr, std::wstring& err) {
    const std::wstring dir = dlc_dir(st, i), tmp = dir + L".tmp";
    Stfs s;
    s.h = CreateFileW(st.found[i].path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s.h == INVALID_HANDLE_VALUE) { err = L"cannot open " + st.found[i].path; return false; }
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
    if (!CreateDirectoryW((st.root + L"\\game").c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { err = L"cannot create game\\"; return false; }
    CreateDirectoryW((st.root + L"\\game\\dlc").c_str(), nullptr);
    if (!CreateDirectoryW(tmp.c_str(), nullptr)) { err = L"cannot create " + tmp; return false; }
    auto make_dirs = [](const std::wstring& d) { for (size_t k = 0; (k = d.find(L'\\', k + 1)) != std::wstring::npos;) CreateDirectoryW(d.substr(0, k).c_str(), nullptr); CreateDirectoryW(d.c_str(), nullptr); };
    std::vector<uint8_t> buf(0x1000 * 64);
    if (pr) pr->line1(std::wstring(L"Unpacking ") + kSetupFiles[i].title + L"...");
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

// Asks for one file until it verifies. False when the player gave up on it.
bool ask_for(State& st, size_t i, const wchar_t* extra) {
    for (;;) {
        std::wstring path;
        if (!browse_file(i, path, extra)) return false;
        bool cancelled = false;
        bool ok;
        {
            Progress pr(L"Checking the file...", true);
            ok = st.try_file(i, path, &pr, &cancelled);
        }
        if (ok) return true;
        if (cancelled) return false;
        uint64_t size = 0, time = 0; file_stat(path, size, time);
        auto it = st.cache.find(lower(path));
        std::wstring got = size != kSetupFiles[i].size ? L"Its size is " + std::to_wstring(size) + L" bytes."
                         : it != st.cache.end() ? L"Its SHA-256 is\n  " + wide(it->second.sha) : L"It could not be read.";
        if (box(std::wstring(L"That is not the file setup needs for ") + kSetupFiles[i].title + L".\n\n" + path + L"\n" + got +
                L"\n\nThe file needed:\n" + needed_text(i) + L"\n\nChoose another file?", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            return false;
    }
}

int run_interactive(State& st) {
    std::wstring intro =
        L"This sets up the Asura's Wrath recomp from your own copy of the Xbox 360 game. You will need to dump these files yourself:\n\n";
    for (size_t i = 0; i < kCount; i++)
        intro += std::wstring(L"  - ") + kSetupFiles[i].title + L" (" + need_text(kSetupFiles[i].need) + L", " + gb(kSetupFiles[i].size) + L")\n";
    intro += L"\nSetup looks for them first (in this folder, gamefiles\\, Xenia's content folder, Downloads, Desktop and Documents), checks each one "
             L"against the known SHA-256, and then asks you for anything it could not find.";
    if (box(intro, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 1;

    if (!quick_search(st)) { st.save(); return 1; }

    // Further searching, as long as something is missing and the player wants to.
    bool searchedDrives = false;
    while (!st.allFound()) {
        std::wstring m = L"Found so far:\n\n" + summary(st, false) + L"\nSearch further for what is missing?\n\n"
                         L"Yes: choose a folder to search\n" + (searchedDrives ? L"No: (all drives were searched)\n" : L"No: search all drives (can take a few minutes)\n") +
                         L"Cancel: go on; you can pick the files yourself next";
        int r = box(m, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDYES) {
            std::wstring dir;
            if (browse_folder(dir) && !search(st, { { dir, 12 } }, L"Searching the folder...")) { st.save(); return 1; }
        } else if (r == IDNO) {
            if (searchedDrives) continue;
            searchedDrives = true;
            if (!search(st, all_drives(), L"Searching all drives...")) { /* cancelled: keep what was found */ }
        } else break;
    }
    st.save();

    // The required files: setup can't finish without them.
    for (size_t i = 0; i < kCount; i++) {
        if (kSetupFiles[i].need != Need::Required || st.found[i].status == Status::Verified) continue;
        std::wstring why = i == 0 ? L"It is the game itself." :
            L"The recomp's program was translated from the game's executable with this update applied, so it only runs with the updated executable.";
        if (box(std::wstring(kSetupFiles[i].title) + L" is required. " + why + L"\n\nThe file needed:\n" + needed_text(i) + L"\n\nBrowse for it now?",
                MB_OKCANCEL | MB_ICONINFORMATION) != IDOK || !ask_for(st, i, L" (required)")) {
            st.save();
            box(std::wstring(L"Setup can't finish without ") + kSetupFiles[i].title + L". Run it again when you have the file.", MB_OK | MB_ICONERROR);
            return 1;
        }
        st.save();
    }

    // Part IV: strongly recommended.
    for (size_t i = 0; i < kCount; i++) {
        if (kSetupFiles[i].need != Need::Recommended || st.found[i].status == Status::Verified) continue;
        std::wstring m = std::wstring(kSetupFiles[i].title) + L" is strongly recommended for the complete experience.\n\n"
                         L"It holds episodes 19 to 22, which follow episode 18 and finish the story: without it the game stops before its real ending.\n\n"
                         L"The file needed:\n" + needed_text(i) + L"\n\nBrowse for it now? (No: continue without it)";
        if (box(m, MB_YESNO | MB_ICONWARNING) == IDYES && ask_for(st, i, L" (strongly recommended; Cancel to skip)")) { st.save(); continue; }
        st.found[i].status = Status::Skipped;
        st.save();
    }

    // The rest: optional, one question for all of them.
    std::wstring optional;
    for (size_t i = 0; i < kCount; i++)
        if (kSetupFiles[i].need == Need::Optional && st.found[i].status != Status::Verified)
            optional += std::wstring(L"  - ") + kSetupFiles[i].title + L": " + kSetupFiles[i].about + L" (" + kSetupFiles[i].usualName + L")\n";
    if (!optional.empty()) {
        bool ask = box(L"These optional DLC episodes were not found:\n\n" + optional + L"\nThe game is complete without them. Browse for them now? "
                       L"(Cancel in the file window skips that one.)", MB_YESNO | MB_ICONQUESTION) == IDYES;
        for (size_t i = 0; i < kCount; i++) {
            if (kSetupFiles[i].need != Need::Optional || st.found[i].status == Status::Verified) continue;
            if (!ask || !ask_for(st, i, L" (optional; Cancel to skip)")) st.found[i].status = Status::Skipped;
            st.save();
        }
    }

    st.save();
    const std::wstring unpacked = install_verified_dlc(st);
    box(L"Your game files are checked:\n\n" + summary(st, false) + (unpacked.empty() ? L"" : L"\nDLC:\n" + unpacked) + L"\nThe list is saved in " + st.sourcesPath +
        L".\n\nTo add DLC later, run setup again. Unpacking the disc and the title update for the recomp is the next step of setup "
        L"(not part of this version yet).", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// Several files at once (OFN_ALLOWMULTISELECT): the folder, then the names, each 0-terminated, an empty name last.
bool browse_files(std::vector<std::wstring>& out) {
    std::vector<wchar_t> buf(1 << 16, 0);
    OPENFILENAMEW o{}; o.lStructSize = sizeof(o);
    o.lpstrFilter = L"All files (*.*)\0*.*\0";
    o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
    o.lpstrTitle = L"Select your Asura's Wrath DLC package files (you can pick several)";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_DONTADDTORECENT | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&o)) return false;
    const std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) { out.push_back(first); return true; }   // one file: the whole path
    for (; *p; p += wcslen(p) + 1) out.push_back(first + L"\\" + p);
    return true;
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

// Adding DLC after setup: pick package files (any names, several at once) or search a folder; each is identified by size and SHA-256 and unpacked
// into game\dlc, where the game finds it on its next start.
int run_add_dlc(State& st) {
    // what was verified before and is still there (instant through the hash cache)
    { Progress pr(L"Checking the files found last time...", true);
      for (size_t i = 0; i < kCount; i++) if (!st.prevPath[i].empty() && exists(st.prevPath[i])) { bool c = false; st.try_file(i, st.prevPath[i], &pr, &c); } }
    std::wstring last;
    for (;;) {
        const std::wstring pending = install_verified_dlc(st);
        st.save();
        if (!pending.empty()) last += pending;
        bool allIn = true; for (size_t i = 0; i < kCount; i++) if (is_dlc(i) && !dlc_installed(st, i)) allIn = false;
        std::wstring m = (last.empty() ? L"" : L"Done:\n" + last + L"\n") + L"DLC:\n" + dlc_status(st);
        if (allIn) { box(m + L"\nEvery DLC is installed.", MB_OK | MB_ICONINFORMATION); return 0; }
        m += L"\nAdd DLC now?\n\nYes: choose the package files (you can pick several; their names don't matter)\nNo: search a folder for them\nCancel: close";
        const int r = box(m, MB_YESNOCANCEL | MB_ICONQUESTION);
        last.clear();
        if (r == IDYES) {
            std::vector<std::wstring> files;
            if (!browse_files(files)) continue;
            Progress pr(L"Checking the files...", true);
            for (const auto& f : files) {
                std::wstring why;
                const int i = identify(st, f, &pr, why);
                if (i < 0) last += L"  - " + file_name(f) + L": " + why + L"\n";
                else if (!is_dlc(i)) last += L"  - " + file_name(f) + L": " + kSetupFiles[i].title + L" (verified; not a DLC)\n";
            }
        } else if (r == IDNO) {
            std::wstring dir;
            if (browse_folder(dir)) search(st, { { dir, 12 } }, L"Searching the folder...");
        } else return 0;
    }
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

    bool check = false, drives = false, full = false, addDlc = false; std::vector<std::wstring> folders, adds; std::wstring report;
    for (int a = 1; a < argc; a++) {
        std::wstring s = argv[a];
        if (s == L"--hash" && a + 1 < argc) {
            uint64_t size = 0, time = 0; std::wstring p = argv[a + 1];
            std::string sha = file_stat(p, size, time) ? file_sha256(p, nullptr) : "";
            printf("%s %llu %s\n", sha.empty() ? "unreadable" : sha.c_str(), (unsigned long long)size, utf8(p).c_str());
            return sha.empty() ? 1 : 0;
        }
        if (s == L"--check") check = true;
        else if (s == L"--full") full = true;
        else if (s == L"--add-dlc") addDlc = true;
        else if (s == L"--add" && a + 1 < argc) adds.push_back(argv[++a]);
        else if (s == L"--drives") drives = true;
        else if (s == L"--search" && a + 1 < argc) folders.push_back(argv[++a]);
        else if (s == L"--report" && a + 1 < argc) report = argv[++a];
        else if (s == L"--root" && a + 1 < argc) { st.root = argv[++a]; st.sourcesPath = st.root + L"\\game\\sources.ini"; }
    }
    st.ui = !check && adds.empty();
    st.load();
    // Setup done before (the disc and the update verified and still there): running it again is for adding DLC, unless --full.
    const bool setUp = !st.prevPath[0].empty() && exists(st.prevPath[0]) && !st.prevPath[1].empty() && exists(st.prevPath[1]);
    int r = !adds.empty() ? run_add_files(st, adds)
          : check ? run_check(st, folders, drives, report)
          : (addDlc || (setUp && !full)) ? run_add_dlc(st)
          : run_interactive(st);
    CoUninitialize();
    return r;
}
