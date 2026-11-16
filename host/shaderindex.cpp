// Shader index: unpacks the game's cooked packages once and lists every shader container in them, so the shader manifest
// (which can ship) only needs to name shaders by a hash of their microcode. See shaderindex.h.
//
// Cooked packages (*.xxx) are UE3 "fully compressed" files: tag 9E2A83C1, block size, total compressed and uncompressed sizes,
// one (compressed, uncompressed) size pair per block, then the blocks. Each block is XMemCompress LZX with a 128 KB window,
// cut into frames of 32 KB output: a frame starts with its compressed size in two big-endian bytes, or with FF, then the
// frame's output size and compressed size in two bytes each. The frames' payloads together are one LZX stream (libmspack).
#include "shaderindex.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <lzx.h>
#include <mspack.h>

namespace {
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

// libmspack reads and writes through these: memory buffers instead of files.
struct MemFile { const uint8_t* src; uint8_t* dst; size_t size, pos; };
int mem_read(mspack_file* f, void* buf, int n) {
    MemFile* m = (MemFile*)f; if (!m->src) return -1;
    const size_t k = std::min((size_t)n, m->size - m->pos); memcpy(buf, m->src + m->pos, k); m->pos += k; return (int)k;
}
int mem_write(mspack_file* f, void* buf, int n) {
    MemFile* m = (MemFile*)f; if (!m->dst) return -1;
    const size_t k = std::min((size_t)n, m->size - m->pos); memcpy(m->dst + m->pos, buf, k); m->pos += k; return (int)k;
}
void* mem_alloc(mspack_system*, size_t n) { return malloc(n); }
void mem_free(void* p) { free(p); }
void mem_copy(void* s, void* d, size_t n) { memmove(d, s, n); }
mspack_system* mem_system() {
    static mspack_system sys = [] { mspack_system s{}; s.read = mem_read; s.write = mem_write; s.alloc = mem_alloc; s.free = mem_free; s.copy = mem_copy; return s; }();
    return &sys;
}

// One block: strip the XMem frame headers, then decode the LZX stream.
bool xmem_lzx_block(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    std::vector<uint8_t> stream; stream.reserve(srcLen);
    size_t at = 0;
    while (at + 2 <= srcLen) {
        uint32_t comp;
        if (src[at] == 0xFF) { if (at + 5 > srcLen) return false; comp = be16(src + at + 3); at += 5; }
        else { comp = be16(src + at); at += 2; }
        if (comp == 0) break;
        if (at + comp > srcLen) return false;
        stream.insert(stream.end(), src + at, src + at + comp); at += comp;
    }
    MemFile in{ stream.data(), nullptr, stream.size(), 0 }, out{ nullptr, dst, dstLen, 0 };
    lzxd_stream* lz = lzxd_init(mem_system(), (mspack_file*)&in, (mspack_file*)&out, 17, 0, 0x8000, (off_t)dstLen, 0);
    if (!lz) return false;
    const int r = lzxd_decompress(lz, (off_t)dstLen);
    lzxd_free(lz);
    if (r != MSPACK_ERR_OK || out.pos != dstLen) {
        static int nl = 0;
        if (nl++ < 5) fprintf(stderr, "[shaderindex] LZX block: error %d, %zu of %zu bytes out, %zu bytes in (%zu after frame headers)\n", r, out.pos, dstLen, srcLen, stream.size());
        return false;
    }
    return true;
}

uint64_t fnv64(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    _fseeki64(f, 0, SEEK_END); const long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)n);
    const bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f); return ok;
}

// Shader containers in a buffer: 102A1100 pixel shader, 102A1101 vertex shader (the same checks as shaderpack.cpp).
template <class F> void scan_containers(const std::vector<uint8_t>& data, F&& found) {
    const size_t size = data.size();
    for (size_t i = 0; i + 36 < size; i++) {
        if (data[i] != 0x10 || data[i + 1] != 0x2A || data[i + 2] != 0x11 || data[i + 3] > 1) continue;
        const uint32_t virt = be32(&data[i + 4]), phys = be32(&data[i + 8]), hdr = be32(&data[i + 24]);
        if (virt == 0 || virt > 0x100000 || phys == 0 || phys > 0x100000 || hdr >= virt || i + hdr + 8 >= size) continue;
        const uint32_t codeOff = be32(&data[i + hdr]), bytes = be32(&data[i + hdr + 4]);
        const uint64_t code = (uint64_t)i + virt + codeOff;
        if (bytes == 0 || (bytes & 3) || bytes > phys || code + bytes > size) continue;
        found(data[i + 3] == 1, &data[(size_t)code], bytes / 4);
    }
}

// Material shaders: a package's shader cache keeps its containers in compressed chunks, each "[uncompressed size]
// [compressed size]" (big-endian) and then an XMem LZX stream of that many bytes (most fit one short frame, FF-headed).
// Found by those two numbers agreeing with the stream's first frame header, then decoded and scanned like any buffer.
template <class F> void scan_shader_chunks(const std::vector<uint8_t>& data, F&& found) {
    std::vector<uint8_t> out;
    const size_t size = data.size();
    for (size_t o = 8; o + 5 < size; o++) {
        const uint32_t u = be32(&data[o - 8]), c = be32(&data[o - 4]);
        if (u < 64 || u > (1u << 20) || c < 8 || c > u + 0x1000 || o + c > size) continue;
        if (data[o] == 0xFF) {                                              // short first frame: its sizes must match
            if (be16(&data[o + 1]) != std::min(u, 0x8000u) || be16(&data[o + 3]) + 5u > c) continue;
        } else {
            if (u < 0x8000 || be16(&data[o]) + 2u > c) continue;           // a full first frame
        }
        out.resize(u);
        if (!xmem_lzx_block(&data[o], c, out.data(), u)) continue;
        scan_containers(out, found);
        o += c - 1;
    }
}

struct Entry { uint64_t key; uint32_t offset, words, isVs; };
std::vector<Entry> g_entries;
std::vector<uint32_t> g_code;
std::unordered_map<uint64_t, size_t> g_byKey;
} // namespace

uint64_t shader_code_key(const uint32_t* words, size_t n, bool isVs) { return fnv64(words, n * 4, isVs ? 0x1234 : 0x5678); }

// Does an XMem LZX stream start at p? Decodes frames until one fails or `want` bytes are out; returns the bytes produced.
size_t xmem_lzx_probe(const uint8_t* p, size_t avail, size_t want, std::vector<uint8_t>& out) {
    out.assign(want, 0);
    size_t lo = 0, hi = want;
    // grow the input frame by frame: decode all frames seen so far each time (simple, and only used for probing)
    std::vector<uint8_t> stream; size_t at = 0, frames = 0;
    while (at + 2 <= avail && frames < want / 0x8000 + 1) {
        uint32_t comp, ulen = 0x8000;
        if (p[at] == 0xFF) { if (at + 5 > avail) break; ulen = be16(p + at + 1); comp = be16(p + at + 3); at += 5; }
        else { comp = be16(p + at); at += 2; }
        if (comp == 0 || comp > 0x8000 + 0x1800 || at + comp > avail || ulen == 0 || ulen > 0x8000) break;
        stream.insert(stream.end(), p + at, p + at + comp); at += comp; frames++;
        const size_t tryOut = std::min(want, frames * 0x8000 - (0x8000 - ulen));
        MemFile in{ stream.data(), nullptr, stream.size(), 0 }, o{ nullptr, out.data(), tryOut, 0 };
        lzxd_stream* lz = lzxd_init(mem_system(), (mspack_file*)&in, (mspack_file*)&o, 17, 0, 0x8000, (off_t)tryOut, 0);
        if (!lz) break;
        const int r = lzxd_decompress(lz, (off_t)tryOut);
        lzxd_free(lz);
        if (r != MSPACK_ERR_OK || o.pos != tryOut) break;
        lo = tryOut;
        if (ulen < 0x8000 || tryOut >= hi) break;
    }
    out.resize(lo);
    return lo;
}

// One compressed chunk at raw[at]: tag, block size, total compressed and uncompressed sizes, a size pair per block, the blocks.
// Appends the unpacked bytes to out; returns the chunk's length in raw, or 0 if it isn't one.
static size_t ue3_unpack_chunk(const std::vector<uint8_t>& raw, size_t at0, std::vector<uint8_t>& out) {
    if (at0 + 16 > raw.size() || be32(&raw[at0]) != 0x9E2A83C1u) return 0;
    const uint32_t block = be32(&raw[at0 + 4]), totalComp = be32(&raw[at0 + 8]), totalRaw = be32(&raw[at0 + 12]);
    if (block == 0 || block > (1u << 24) || (block & 0xFFF) || totalRaw == 0 || totalRaw > (1u << 30)) return 0;   // a package header has other fields here
    const size_t blocks = ((size_t)totalRaw + block - 1) / block;
    size_t at = at0 + 16 + blocks * 8;
    if (at > raw.size() || totalComp > raw.size() - at) return 0;
    const size_t w0 = out.size();
    out.resize(w0 + totalRaw);
    size_t w = w0;
    for (size_t b = 0; b < blocks; b++) {
        const uint32_t c = be32(&raw[at0 + 16 + b * 8]), u = be32(&raw[at0 + 16 + b * 8 + 4]);
        if (at + c > raw.size() || w + u > out.size() || !xmem_lzx_block(&raw[at], c, &out[w], u)) { out.resize(w0); return 0; }
        at += c; w += u;
    }
    if (w != w0 + totalRaw) { out.resize(w0); return 0; }
    return at - at0;
}
bool ue3_unpack_file(const std::string& path, std::vector<uint8_t>& out) {
    std::vector<uint8_t> raw;
    out.clear();
    if (!read_file(path, raw) || raw.size() < 16 || be32(&raw[0]) != 0x9E2A83C1u) return false;
    if (ue3_unpack_chunk(raw, 0, out)) return true;                          // fully compressed file
    // A regular package: the summary is plain, the body is compressed chunks of the same layout. Unpack each one found
    // (their order in out doesn't matter for scanning), after the plain part.
    out.assign(raw.begin(), raw.end());
    bool any = false;
    for (size_t i = 4; i + 16 <= raw.size(); i++) {
        if (raw[i] != 0x9E || be32(&raw[i]) != 0x9E2A83C1u) continue;
        const size_t n = ue3_unpack_chunk(raw, i, out);
        if (n) { any = true; i += n - 1; }
    }
    return any;
}

bool shaderindex_build(const std::string& cookedDir, const std::string& outDir, const std::function<void(int, int)>& progress) {
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((cookedDir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string n = fd.cFileName;
        const size_t dot = n.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : n.substr(dot);
        if (ext == ".xxx" || ext == ".bin" || ext == ".upk") files.push_back(n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());

    std::vector<Entry> entries; std::vector<uint32_t> code; std::unordered_map<uint64_t, size_t> seen;
    std::vector<uint8_t> data; std::vector<uint32_t> words;
    int failed = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (progress) progress((int)i, (int)files.size());
        const std::string path = cookedDir + "\\" + files[i];
        if (!ue3_unpack_file(path, data)) {
            if (!read_file(path, data)) { failed++; continue; }          // not fully compressed: scan it as it is
        }
        auto add = [&](bool isVs, const uint8_t* p, uint32_t n) {
            words.resize(n);
            for (uint32_t k = 0; k < n; k++) words[k] = be32(p + k * 4);
            const uint64_t key = shader_code_key(words.data(), n, isVs);
            if (seen.count(key)) return;
            seen[key] = entries.size();
            entries.push_back({ key, (uint32_t)code.size(), n, isVs ? 1u : 0u });
            code.insert(code.end(), words.begin(), words.end());
        };
        scan_containers(data, add);                                          // global shader cache: plain containers
        scan_shader_chunks(data, add);                                       // material shader caches: compressed chunks
    }
    if (progress) progress((int)files.size(), (int)files.size());
    CreateDirectoryA(outDir.c_str(), nullptr);
    FILE* fi = fopen((outDir + "\\shader_index.bin").c_str(), "wb");
    FILE* fc = fopen((outDir + "\\shader_code.bin").c_str(), "wb");
    if (!fi || !fc) { if (fi) fclose(fi); if (fc) fclose(fc); return false; }
    const uint32_t magic = 0x58495341, count = (uint32_t)entries.size();      // "ASIX"
    fwrite(&magic, 4, 1, fi); fwrite(&count, 4, 1, fi);
    fwrite(entries.data(), sizeof(Entry), entries.size(), fi);
    fwrite(code.data(), 4, code.size(), fc);
    fclose(fi); fclose(fc);
    fprintf(stderr, "[shaderindex] %zu files, %d unreadable, %zu shaders (%zu words)\n", files.size(), failed, entries.size(), code.size());
    return true;
}

bool shaderindex_load(const std::string& outDir) {
    std::vector<uint8_t> idx, cb;
    if (!read_file(outDir + "\\shader_index.bin", idx) || !read_file(outDir + "\\shader_code.bin", cb) || idx.size() < 8) return false;
    uint32_t magic, count; memcpy(&magic, &idx[0], 4); memcpy(&count, &idx[4], 4);
    if (magic != 0x58495341 || idx.size() != 8 + (size_t)count * sizeof(Entry)) return false;
    g_entries.resize(count); memcpy(g_entries.data(), &idx[8], (size_t)count * sizeof(Entry));
    g_code.resize(cb.size() / 4); memcpy(g_code.data(), cb.data(), g_code.size() * 4);
    g_byKey.clear();
    for (size_t i = 0; i < g_entries.size(); i++) {
        const Entry& e = g_entries[i];
        if ((size_t)e.offset + e.words <= g_code.size()) g_byKey[e.key] = i;
    }
    return true;
}
bool shaderindex_find(uint64_t key, std::vector<uint32_t>& code) {
    auto it = g_byKey.find(key);
    if (it == g_byKey.end()) return false;
    const Entry& e = g_entries[it->second];
    code.assign(g_code.begin() + e.offset, g_code.begin() + e.offset + e.words);
    return true;
}
size_t shaderindex_size() { return g_byKey.size(); }
void shaderindex_for_each(bool isVs, const std::function<void(uint64_t, const std::vector<uint32_t>&)>& fn) {
    std::vector<uint32_t> code;
    for (const auto& kv : g_byKey) {
        const Entry& e = g_entries[kv.second];
        if ((e.isVs != 0) != isVs) continue;
        code.assign(g_code.begin() + e.offset, g_code.begin() + e.offset + e.words);
        fn(kv.first, code);
    }
}

bool shaderindex_closest(const std::vector<uint32_t>& code, bool isVs, uint64_t& key, std::vector<uint32_t>& base) {
    // by kind and length: built on first use
    static std::unordered_map<uint64_t, std::vector<size_t>> bySize;
    static size_t builtFor = 0;
    if (builtFor != g_entries.size()) {
        bySize.clear();
        for (size_t i = 0; i < g_entries.size(); i++) bySize[(uint64_t)g_entries[i].words << 1 | g_entries[i].isVs].push_back(i);
        builtFor = g_entries.size();
    }
    auto it = bySize.find((uint64_t)code.size() << 1 | (isVs ? 1 : 0));
    if (it == bySize.end()) return false;
    size_t best = (size_t)-1, bestDiff = (size_t)-1;
    for (size_t i : it->second) {
        const uint32_t* w = &g_code[g_entries[i].offset];
        size_t diff = 0;
        for (size_t k = 0; k < code.size() && diff < bestDiff; k++) if (w[k] != code[k]) diff++;
        if (diff < bestDiff) { bestDiff = diff; best = i; if (!diff) break; }
    }
    if (best == (size_t)-1 || bestDiff > code.size() / 4) return false;       // only a few words patched, or it's another shader
    key = g_entries[best].key;
    base.assign(g_code.begin() + g_entries[best].offset, g_code.begin() + g_entries[best].offset + code.size());
    return true;
}
