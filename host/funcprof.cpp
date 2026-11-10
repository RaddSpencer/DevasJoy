// Profiling build only: call counts of every recompiled game function (see funcprof.h). funcprof_frame() runs at each swap;
// for the frames in ASURA_FUNCPROF="first,last" it writes "[prof] f=<frame> <function> <calls>" for every function called.
#ifdef ASURA_FUNCPROF
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
struct Slot { const char* name; uint32_t count; };
std::mutex g_mtx;
std::vector<Slot*> g_slots;
}
extern "C" uint32_t* asura_prof_slot(const char* name) {
    std::lock_guard<std::mutex> lock(g_mtx);
    Slot* s = new Slot{ name, 0 };
    g_slots.push_back(s);
    return &s->count;
}
void funcprof_frame(uint64_t frame) {
    static long long first = -1, last = -1;
    static bool init = false;
    if (!init) { init = true; if (const char* e = getenv("ASURA_FUNCPROF")) sscanf(e, "%lld,%lld", &first, &last); }
    std::lock_guard<std::mutex> lock(g_mtx);
    const bool want = first >= 0 && (long long)frame >= first && (long long)frame <= last;
    for (Slot* s : g_slots) {
        if (want && s->count) fprintf(stderr, "[prof] f=%llu %s %u\n", (unsigned long long)frame, s->name, s->count);
        s->count = 0;
    }
}
#else
#include <cstdint>
void funcprof_frame(uint64_t) {}
#endif
