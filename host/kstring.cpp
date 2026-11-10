// xboxkrnl string helpers and the printf family (sprintf, _snprintf, _vsnprintf, DbgPrint).
// Guest variadic arguments are 8-byte slots: registers r3..r10 first, then the caller's parameter area at r1+0x10+8*i.
// A va_list is a guest pointer to such slots. 32-bit values sit in the low word; doubles are passed as raw bits in the slot.
#include "kernel.h"
#include <windows.h>
#include <string>

struct ArgSource {
    PPCContext* ctx = nullptr; uint32_t index = 0;   // register/stack arguments from this parameter index
    uint32_t va = 0;                                 // or a va_list pointer
    uint64_t next() {
        if (va) { uint64_t v = rd64(va); va += 8; return v; }
        uint32_t i = index++;
        switch (i) {   // PPCContext keeps r3 apart from r4..r10, so no indexing
          case 0: return ctx->r3.u64; case 1: return ctx->r4.u64; case 2: return ctx->r5.u64; case 3: return ctx->r6.u64;
          case 4: return ctx->r7.u64; case 5: return ctx->r8.u64; case 6: return ctx->r9.u64; case 7: return ctx->r10.u64;
        }
        return rd64(ctx->r1.u32 + 0x10 + i * 8);
    }
};
static std::string wide_to_utf8(uint32_t p, int maxChars) {
    std::wstring w;
    for (int i = 0; p && (maxChars < 0 || i < maxChars); i++) { uint16_t c = rd16(p + i * 2); if (!c) break; w += (wchar_t)c; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// Formats one guest printf string. Widths, precision, '*', the h/l/ll/I64/z length forms and %d i u x X o c s S p f e g a n %.
std::string guest_format(uint32_t fmtPtr, ArgSource& a, bool wide = false) {
    std::string out;
    auto ch = [&](uint32_t i) -> uint32_t { return wide ? rd16(fmtPtr + i * 2) : g_base[fmtPtr + i]; };
    for (uint32_t i = 0; ch(i); i++) {
        uint32_t c = ch(i);
        if (c != '%') { out += (char)c; continue; }
        std::string spec = "%";
        i++;
        while (strchr("-+ #0", (int)ch(i)) && ch(i)) spec += (char)ch(i++);
        if (ch(i) == '*') { spec += std::to_string((int32_t)a.next()); i++; } else while (isdigit((int)ch(i))) spec += (char)ch(i++);
        if (ch(i) == '.') { spec += '.'; i++; if (ch(i) == '*') { spec += std::to_string((int32_t)a.next()); i++; } else while (isdigit((int)ch(i))) spec += (char)ch(i++); }
        int len = 0;   // 0 int, 1 short, 2 long, 3 64-bit
        for (;;) {
            uint32_t l = ch(i);
            if (l == 'h') { len = 1; i++; } else if (l == 'l') { len = (ch(i + 1) == 'l') ? 3 : 2; i += (len == 3) ? 2 : 1; }
            else if (l == 'I' && ch(i + 1) == '6' && ch(i + 2) == '4') { len = 3; i += 3; } else if (l == 'z' || l == 'L' || l == 'q') { i++; }
            else break;
        }
        uint32_t conv = ch(i);
        char buf[512];
        switch (conv) {
          case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = a.next();
            std::string s = spec + "ll" + (char)conv;
            if (len == 3) snprintf(buf, sizeof buf, s.c_str(), v);
            else if (conv == 'd' || conv == 'i') snprintf(buf, sizeof buf, s.c_str(), (long long)(len == 1 ? (int16_t)v : (int32_t)v));
            else snprintf(buf, sizeof buf, s.c_str(), (unsigned long long)(len == 1 ? (uint16_t)v : (uint32_t)v));
            out += buf; break; }
          case 'c': case 'C': {
            uint32_t v = (uint32_t)a.next();
            if ((conv == 'C') != wide && len != 1) { std::string u = v < 0x80 ? std::string(1, (char)v) : "?"; out += u; } else out += (char)v;
            break; }
          case 's': case 'S': {
            uint32_t p = (uint32_t)a.next();
            bool w = (conv == 'S') ? !wide : wide;
            if (len == 2) w = true; else if (len == 1) w = false;
            std::string s = !p ? "(null)" : (w ? wide_to_utf8(p, -1) : guest_cstr(p, 1 << 16));
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += (s.size() >= sizeof buf - 1) ? s : buf; break; }
          case 'p': snprintf(buf, sizeof buf, "%08X", (uint32_t)a.next()); out += buf; break;
          case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = a.next(); double d; memcpy(&d, &bits, 8);
            snprintf(buf, sizeof buf, (spec + (char)conv).c_str(), d); out += buf; break; }
          case 'n': a.next(); break;
          case '%': out += '%'; break;
          case 0: return out;
          default: out += spec; out += (char)conv; break;
        }
    }
    return out;
}
static uint32_t put_result(uint32_t dst, int64_t cap, const std::string& s) {
    if (!dst) return (uint32_t)s.size();
    size_t n = cap < 0 ? s.size() : std::min<size_t>(s.size(), (size_t)cap);
    memcpy(g_base + dst, s.data(), n);
    if (cap < 0 || n < (size_t)cap) g_base[dst + n] = 0;
    return (uint32_t)n;
}
// sprintf(buffer, format, ...)
static void sprintf_(PPCContext& ctx, uint8_t*) {
    ArgSource a; a.ctx = &ctx; a.index = 2;
    std::string s = guest_format(ctx.r4.u32, a);
    ctx.r3.u64 = put_result(ctx.r3.u32, -1, s);
}
// _snprintf(buffer, count, format, ...): -1 when it doesn't fit
static void snprintf_(PPCContext& ctx, uint8_t*) {
    ArgSource a; a.ctx = &ctx; a.index = 3;
    std::string s = guest_format(ctx.r5.u32, a);
    put_result(ctx.r3.u32, ctx.r4.u32, s);
    ctx.r3.s64 = s.size() > ctx.r4.u32 ? -1 : (int64_t)s.size();
}
// _vsnprintf(buffer, count, format, va_list)
static void vsnprintf_(PPCContext& ctx, uint8_t*) {
    ArgSource a; a.va = ctx.r6.u32;
    std::string s = guest_format(ctx.r5.u32, a);
    put_result(ctx.r3.u32, ctx.r4.u32, s);
    ctx.r3.s64 = s.size() > ctx.r4.u32 ? -1 : (int64_t)s.size();
}
static void DbgPrint_(PPCContext& ctx, uint8_t*) {
    ArgSource a; a.ctx = &ctx; a.index = 1;
    std::string s = guest_format(ctx.r3.u32, a);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (g_logging) fprintf(stderr, "[dbg] %s\n", s.c_str());
    ctx.r3.u64 = 0;
}

// ---- NT strings: ANSI_STRING / UNICODE_STRING {u16 length; u16 maximum; u32 buffer} ----
static void RtlInitAnsiString_(PPCContext& ctx, uint8_t*) {
    uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    uint16_t n = s ? (uint16_t)strlen((const char*)gp(s)) : 0;
    wr16(d, n); wr16(d + 2, s ? n + 1 : 0); wr32(d + 4, s);
}
static void RtlInitUnicodeString_(PPCContext& ctx, uint8_t*) {
    uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    uint16_t n = 0;
    if (s) while (rd16(s + n)) n += 2;
    wr16(d, n); wr16(d + 2, s ? n + 2 : 0); wr32(d + 4, s);
}
uint32_t pool_alloc_public(uint32_t size);
static void RtlFreeAnsiString_(PPCContext& ctx, uint8_t*) { if (ctx.r3.u32) { wr16(ctx.r3.u32, 0); wr32(ctx.r3.u32 + 4, 0); } }
// RtlUnicodeStringToAnsiString(ansi*, unicode*, allocate)
static void RtlUnicodeStringToAnsiString_(PPCContext& ctx, uint8_t*) {
    uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    uint16_t n = rd16(s) / 2; uint32_t src = rd32(s + 4);
    uint32_t buf;
    if (ctx.r5.u32) { buf = vm_alloc(n + 1); wr16(d + 2, n + 1); wr32(d + 4, buf); }
    else { buf = rd32(d + 4); if (rd16(d + 2) < n + 1) { ctx.r3.u64 = STATUS_BUFFER_OVERFLOW; return; } }
    for (uint16_t i = 0; i < n; i++) { uint16_t c = rd16(src + i * 2); g_base[buf + i] = c < 0x100 ? (uint8_t)c : '?'; }
    g_base[buf + n] = 0;
    wr16(d, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// RtlMultiByteToUnicodeN(dst, dst_bytes, written*, src, src_bytes)
static void RtlMultiByteToUnicodeN_(PPCContext& ctx, uint8_t*) {
    uint32_t d = ctx.r3.u32, cap = ctx.r4.u32 / 2, src = ctx.r6.u32, n = ctx.r7.u32;
    uint32_t k = std::min(cap, n);
    for (uint32_t i = 0; i < k; i++) wr16(d + i * 2, g_base[src + i]);
    if (ctx.r5.u32) wr32(ctx.r5.u32, k * 2);
    ctx.r3.u64 = STATUS_SUCCESS;
}
// RtlUnicodeToMultiByteN(dst, dst_bytes, written*, src, src_bytes)
static void RtlUnicodeToMultiByteN_(PPCContext& ctx, uint8_t*) {
    uint32_t d = ctx.r3.u32, cap = ctx.r4.u32, src = ctx.r6.u32, n = ctx.r7.u32 / 2;
    uint32_t k = std::min(cap, n);
    for (uint32_t i = 0; i < k; i++) { uint16_t c = rd16(src + i * 2); g_base[d + i] = c < 0x100 ? (uint8_t)c : '?'; }
    if (ctx.r5.u32) wr32(ctx.r5.u32, k);
    ctx.r3.u64 = STATUS_SUCCESS;
}
static void RtlUpcaseUnicodeChar_(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = (uint16_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)(uint16_t)ctx.r3.u32); }
// RtlCompareStringN(s1, len1, s2, len2, case_insensitive)
static void RtlCompareStringN_(PPCContext& ctx, uint8_t*) {
    uint32_t a = ctx.r3.u32, b = ctx.r5.u32, n = std::min(ctx.r4.u32, ctx.r6.u32);
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++) {
        int x = g_base[a + i], y = g_base[b + i];
        if (ctx.r7.u32) { x = toupper(x); y = toupper(y); }
        r = x - y;
    }
    if (!r) r = (int)ctx.r4.u32 - (int)ctx.r6.u32;
    ctx.r3.s64 = r;
}
// RtlCompareMemoryUlong(source, length, pattern) -> bytes that match
static void RtlCompareMemoryUlong_(PPCContext& ctx, uint8_t*) {
    uint32_t s = ctx.r3.u32, n = ctx.r4.u32 & ~3u, p = ctx.r5.u32, i = 0;
    while (i < n && rd32(s + i) == p) i += 4;
    ctx.r3.u64 = i;
}
static void RtlFillMemoryUlong_(PPCContext& ctx, uint8_t*) {
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4) wr32(ctx.r3.u32 + i, ctx.r5.u32);
}
// NtStatus -> Win32 error
static void RtlNtStatusToDosError_(PPCContext& ctx, uint8_t*) {
    uint32_t s = ctx.r3.u32, e;
    switch (s) {
      case 0: e = 0; break;
      case STATUS_NO_MORE_FILES: e = 18; break;
      case STATUS_NO_SUCH_FILE: case STATUS_OBJECT_NAME_NOT_FOUND: e = 2; break;
      case STATUS_OBJECT_PATH_NOT_FOUND: e = 3; break;
      case STATUS_END_OF_FILE: e = 38; break;
      case STATUS_ACCESS_DENIED: e = 5; break;
      case STATUS_OBJECT_NAME_COLLISION: e = 183; break;
      case STATUS_INVALID_PARAMETER: e = 87; break;
      case STATUS_INVALID_HANDLE: e = 6; break;
      case STATUS_NO_MEMORY: e = 8; break;
      case STATUS_PENDING: e = 997; break;
      case STATUS_TIMEOUT: e = 1460; break;
      case STATUS_BUFFER_OVERFLOW: e = 234; break;
      case STATUS_BUFFER_TOO_SMALL: e = 122; break;
      default: e = (s & 0xFFFF0000u) == 0x80070000u ? (s & 0xFFFF) : 317;
    }
    ctx.r3.u64 = e;
}

void register_string_imports() {
    register_import("sprintf", sprintf_);
    register_import("_snprintf", snprintf_);
    register_import("_vsnprintf", vsnprintf_);
    register_import("DbgPrint", DbgPrint_);
    register_import("RtlInitAnsiString", RtlInitAnsiString_);
    register_import("RtlInitUnicodeString", RtlInitUnicodeString_);
    register_import("RtlFreeAnsiString", RtlFreeAnsiString_);
    register_import("RtlUnicodeStringToAnsiString", RtlUnicodeStringToAnsiString_);
    register_import("RtlMultiByteToUnicodeN", RtlMultiByteToUnicodeN_);
    register_import("RtlUnicodeToMultiByteN", RtlUnicodeToMultiByteN_);
    register_import("RtlUpcaseUnicodeChar", RtlUpcaseUnicodeChar_);
    register_import("RtlCompareStringN", RtlCompareStringN_);
    register_import("RtlCompareMemoryUlong", RtlCompareMemoryUlong_);
    register_import("RtlFillMemoryUlong", RtlFillMemoryUlong_);
    register_import("RtlNtStatusToDosError", RtlNtStatusToDosError_);
}
