// Fixes for races in the game that the console's timing hid and a fast PC exposes.
// Addresses refer to Tekken Tag Tournament 2 (USA, Europe), retail or with Title Update 3
// (TT2_TU3); see game/tt2_frame.cpp.

#include "../guest.h"

#include <atomic>

using namespace tt2;

PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);

#if TT2_TU3
#define TT2_HAVOK_JOB_WAIT sub_824F6C28
#define TT2_HAVOK_JOB_WAIT_IMPL __imp__sub_824F6C28
constexpr uint32_t kHavokAfterWait = 0x824F58B8;
#else
#define TT2_HAVOK_JOB_WAIT sub_824E2E48
#define TT2_HAVOK_JOB_WAIT_IMPL __imp__sub_824E2E48
constexpr uint32_t kHavokAfterWait = 0x824E1AD0;
#endif

namespace
{
    // The kernel's critical section calls, made directly (not through the import thunks,
    // whose addresses differ between versions).
    void enter(PPCContext& ctx, uint8_t* base, uint32_t cs)
    {
        PPCContext c = ctx;
        c.r3.u64 = cs;
        __imp__RtlEnterCriticalSection(c, base);
    }
    void leave(PPCContext& ctx, uint8_t* base, uint32_t cs)
    {
        PPCContext c = ctx;
        c.r3.u64 = cs;
        __imp__RtlLeaveCriticalSection(c, base);
    }
}

// sub_824E2E48 / TU3 sub_824F6C28 (Havok job wait). Called with the job queue's critical section
// ([this+148]+8) held:
//
//   if (state == 0) return 1;
//   if (state == 2) this->vfunc12();        // busy-waits until the worker finishes the job
//   Leave(cs); sub_824E1AD0(this); Enter(cs); return 1;      (TU3: sub_824F58B8)
//
// In state 2 the busy-wait runs with the lock still held. A worker that needs the same lock to
// finish its job then deadlocks with the main thread. On the console the worker is nearly always
// done before the main thread gets there; on a fast PC it hangs about every other boot. The
// wait below runs with the lock released, which is what the function does for the rest of its
// waiting anyway.
PPC_FUNC_IMPL(TT2_HAVOK_JOB_WAIT_IMPL);
PPC_FUNC(TT2_HAVOK_JOB_WAIT)
{
    const uint32_t self = ctx.r3.u32;
    if (load32(self + 144) != 2)
    {
        TT2_HAVOK_JOB_WAIT_IMPL(ctx, base);
        return;
    }

    static std::atomic<bool> logged{ false };
    if (!logged.exchange(true))
        tt2::log("fix: Havok job wait runs without the queue lock");

    const uint32_t cs = load32(self + 148) + 8;
    leave(ctx, base, cs);
    tt2::call_guest(load32(load32(self) + 12), self); // the busy-wait, now without the lock
    enter(ctx, base, cs);

    leave(ctx, base, cs);
    tt2::call_guest(kHavokAfterWait, self);
    enter(ctx, base, cs);
    ctx.r3.u64 = 1;
}
