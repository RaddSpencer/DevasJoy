// Shared declarations for the Asura host: guest memory access, the import registry, handles and logging.
#pragma once
#include "ppc_config.h"
#include "ppc_context.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern uint8_t* g_base;                  // guest address 0 in host memory
extern thread_local PPCContext* g_ctx;   // the running guest thread's context (for crash reports)
extern bool g_logging;

#define LOG(...) do { if (g_logging) fprintf(stderr, __VA_ARGS__); } while (0)

// ---- big-endian guest memory ----
inline uint8_t*  gp(uint32_t a) { return g_base + a; }
inline uint32_t  rd32(uint32_t a) { return _byteswap_ulong(*(uint32_t*)(g_base + a)); }
inline uint16_t  rd16(uint32_t a) { return _byteswap_ushort(*(uint16_t*)(g_base + a)); }
inline uint64_t  rd64(uint32_t a) { return _byteswap_uint64(*(uint64_t*)(g_base + a)); }
inline void      wr32(uint32_t a, uint32_t v) { *(uint32_t*)(g_base + a) = _byteswap_ulong(v); }
inline void      wr16(uint32_t a, uint16_t v) { *(uint16_t*)(g_base + a) = _byteswap_ushort(v); }
inline void      wr64(uint32_t a, uint64_t v) { *(uint64_t*)(g_base + a) = _byteswap_uint64(v); }
std::string      guest_cstr(uint32_t a, size_t max = 1024);
std::string      guest_ansi_string(uint32_t ansiStringPtr);   // {u16 len; u16 max; u32 buffer}

// ---- imports ----
using ImportFn = void (*)(PPCContext&, uint8_t*);
struct ImportEntry { uint32_t slot, thunk; int lib, ordinal; const char* name; };
const ImportEntry* host_import_table(size_t& count);
void register_import(const char* name, ImportFn fn);   // warns when the title doesn't import it
void register_kernel_imports();
void register_xam_imports();
void register_gpu_imports();
void register_audio_imports();

// Fills the import variables (KeTimeStampBundle, XboxHardwareInfo, ...) once the slots point at their blocks.
void kernel_init_variables();
uint32_t import_variable(const char* name);            // guest address of an imported variable's block, 0 if not imported

// ---- status codes ----
constexpr uint32_t STATUS_SUCCESS = 0, STATUS_PENDING = 0x103, STATUS_TIMEOUT = 0x102, STATUS_ABANDONED = 0x80;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001, STATUS_NOT_IMPLEMENTED = 0xC0000002, STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008, STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011, STATUS_NO_MEMORY = 0xC0000017, STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_BUFFER_TOO_SMALL = 0xC0000023, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A, STATUS_MUTANT_NOT_OWNED = 0xC0000046, STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006, STATUS_BUFFER_OVERFLOW = 0x80000005;
constexpr uint32_t ERROR_IO_PENDING = 997, ERROR_NO_MORE_FILES = 18, ERROR_NOT_FOUND = 0x490, ERROR_DEVICE_NOT_CONNECTED = 0x48F;

// ---- guest memory allocation (kernel side) ----
uint32_t vm_alloc(uint32_t size);           // committed, 64K aligned, from the virtual arena
uint32_t phys_alloc(uint32_t size, uint32_t align);

// ---- dispatcher objects (guest memory) and handles ----
// Every waitable object is a guest X_DISPATCHER_HEADER {u8 type; u8 absolute; u8 size; u8 inserted; i32 signal_state; list}.
// Types: 0 notification event, 1 synchronization event, 2 mutant, 5 semaphore, 6 thread, 8/9 timer.
uint32_t ob_create(uint8_t type, int32_t state, uint32_t size = 0x40);   // a new object in guest memory
void     ob_signal_changed();               // wake waiters after any signal state change
uint32_t handle_create(uint32_t obj);       // a guest handle naming obj
uint32_t handle_object(uint32_t handle);    // 0 if unknown
void     handle_close(uint32_t handle);
// Waits on guest objects (pointers). Returns an NTSTATUS: index (wait-any), 0 (wait-all), STATUS_TIMEOUT.
uint32_t ke_wait(const uint32_t* objs, uint32_t count, bool waitAll, int64_t timeout100ns, bool hasTimeout, bool alertable = false);
void apc_queue(uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3);   // runs at this thread's next alertable wait
int64_t  guest_timeout(uint32_t ptr, bool& has);   // reads an LARGE_INTEGER* timeout (null = infinite)

// ---- threads ----
uint32_t thread_current_object();           // the running guest thread's KTHREAD
void     thread_run_guest(uint32_t entry, uint32_t r3, uint32_t r4);   // calls a guest function on this host thread (host-made threads)
// A host thread that calls into guest code (vsync, interrupts, audio) needs its own PCR, thread object and stack.
struct GuestHostThread { uint32_t pcr, kthread, stackTop; };
GuestHostThread make_host_guest_thread(const char* name, uint32_t stackSize = 0x40000);
void bind_guest_thread(PPCContext& ctx, const GuestHostThread& t);
void call_guest(PPCContext& ctx, uint32_t fn);   // ctx registers already set

// ---- paths ----
// A problem the player should see (crash, fatal error, no Direct3D 11): writes logs\<kind>_<date>.txt with title and details (plus anything the
// writer callback adds), shows a message naming the file, and ends the process. Safe to call from any thread, once.
void report_problem(const char* kind, const std::string& title, const std::string& details, void (*extra)(FILE*, void*) = nullptr, void* arg = nullptr);
std::string data_root();    // folder holding game\, saves\ (C:\dev\asura in the development layout)
bool resolve_game_path(const std::string& guestPath, std::string& hostPath, bool& writable);
void content_mount(const std::string& root, const std::string& hostDir, bool writable);
void content_unmount(const std::string& root);

// ---- async completion (XOVERLAPPED) ----
void complete_overlapped(uint32_t ov, uint32_t status, uint32_t length = 0);
void event_set_by_handle(uint32_t handle);
