#pragma once
// Profiling build only (cmake -DASURA_FUNCPROF=ON, a separate build folder): every recompiled game function counts its calls.
// Force-included before the generated code, so it replaces ppc_context.h's PPC_FUNC_PROLOGUE. funcprof.cpp writes the counts
// of chosen frames to the log (ASURA_FUNCPROF="first,last" frames), to compare frames where something is drawn and where it isn't.
#include <cstdint>
#include <cstddef>
extern "C" uint32_t* asura_prof_slot(const char* name);
#define PPC_FUNC_PROLOGUE() \
    static uint32_t* const prof_slot_ = asura_prof_slot(__func__); \
    ++*prof_slot_; \
    __builtin_assume(((size_t)base & 0x1F) == 0)
