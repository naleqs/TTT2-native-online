#pragma once

// Force-included into the recompiled game code (see src/runtime/CMakeLists.txt).
//
// 1. XenonRecomp translates `mftb` (read the time base) into __rdtsc(), which runs at the host
//    TSC rate. The game expects the console's 50 MHz time base, and rollback needs a clock it
//    can control during re-simulated frames, so the call is redirected to the runtime.
// 2. Indirect calls (function pointers, virtual calls): a target the recompiler did not see as
//    a function would jump to a null host pointer. Report the guest address instead.

#include <immintrin.h>
#include <x86intrin.h>

#include <cstdint>

extern "C" uint64_t tt2_guest_timebase();
#define __rdtsc() tt2_guest_timebase()

extern "C" [[noreturn]] void tt2_missing_function(uint32_t target, uint64_t lr);
#define PPC_CALL_INDIRECT_FUNC(x)                            \
    do                                                       \
    {                                                        \
        const uint32_t tt2_target = (x);                     \
        PPCFunc* tt2_fn = PPC_LOOKUP_FUNC(base, tt2_target); \
        if (tt2_fn) [[likely]]                               \
            tt2_fn(ctx, base);                               \
        else                                                 \
            tt2_missing_function(tt2_target, ctx.lr);        \
    } while (0)
