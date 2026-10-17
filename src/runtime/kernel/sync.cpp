// Events, semaphores, mutants, timers, waits, DPCs, spinlocks and critical sections.

#include "objects.h"
#include "threads.h"
#include "../memory.h"

#include <immintrin.h>
#include <thread>

using namespace tt2;
using namespace tt2::kernel;

namespace
{
    void write_handle(const Args& a, int index, std::shared_ptr<Object> obj)
    {
        if (auto* h = a.ptr<be<uint32_t>>(index))
            *h = create_handle(std::move(obj));
    }

    template <typename T>
    void write_out(const Args& a, int index, T value)
    {
        if (auto* p = a.ptr<be<T>>(index))
            *p = value;
    }

    uint32_t set_event(Event& e)
    {
        uint32_t previous = 0;
        with_lock([&] {
            previous = e.signaled;
            e.signaled = true;
            e.mirror_signal_state(1);
        });
        return previous;
    }

    uint32_t reset_event(Event& e)
    {
        uint32_t previous = 0;
        with_lock([&] {
            previous = e.signaled;
            e.signaled = false;
            e.mirror_signal_state(0);
        });
        return previous;
    }

    uint32_t wait_one(std::shared_ptr<Object> obj, const be<int64_t>* timeout, bool alertable)
    {
        if (!obj)
            return status::invalid_handle;
        return wait({ obj }, false, deadline_from_nt(timeout), alertable);
    }

    // Thread identity used by critical sections and spinlocks: the KTHREAD pointer (PCR+0x100).
    uint32_t owner_id(const PPCContext& ctx) { return load32(ctx.r13.u32 + 0x100); }

    bool cas32(uint32_t guest, uint32_t expected, uint32_t desired)
    {
        uint32_t e = byteswap(expected);
        return __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(g_base + guest), &e, byteswap(desired), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    void backoff(int& spins)
    {
        if (++spins < 64)
            _mm_pause();
        else if (spins < 256)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// ---------------------------------------------------------------------- events

TT2_IMPORT(NtCreateEvent)
{
    Args a{ ctx };
    // EVENT_TYPE: 0 = notification (manual reset), 1 = synchronization (auto reset)
    write_handle(a, 0, std::make_shared<Event>(a.u32(2) == 0, a.u32(3) != 0));
    a.ret(status::success);
}

TT2_IMPORT(NtSetEvent)
{
    Args a{ ctx };
    auto e = handle_as<Event>(a.u32(0));
    if (!e)
        return a.ret(status::invalid_handle);
    write_out<int32_t>(a, 1, int32_t(set_event(*e)));
    a.ret(status::success);
}

TT2_IMPORT(NtClearEvent)
{
    Args a{ ctx };
    auto e = handle_as<Event>(a.u32(0));
    if (!e)
        return a.ret(status::invalid_handle);
    reset_event(*e);
    a.ret(status::success);
}

TT2_IMPORT(NtPulseEvent)
{
    Args a{ ctx };
    auto e = handle_as<Event>(a.u32(0));
    if (!e)
        return a.ret(status::invalid_handle);
    // Wake current waiters, then reset. With the condition-variable model a waiter must
    // observe the signal, so keep it set for a moment.
    const uint32_t previous = set_event(*e);
    std::this_thread::yield();
    reset_event(*e);
    write_out<int32_t>(a, 1, int32_t(previous));
    a.ret(status::success);
}

TT2_IMPORT(KeSetEvent)
{
    Args a{ ctx };
    auto e = std::dynamic_pointer_cast<Event>(object_from_pointer(a.u32(0)));
    a.ret(e ? set_event(*e) : 0);
}

TT2_IMPORT(KeResetEvent)
{
    Args a{ ctx };
    auto e = std::dynamic_pointer_cast<Event>(object_from_pointer(a.u32(0)));
    a.ret(e ? reset_event(*e) : 0);
}

// ---------------------------------------------------------------------- semaphores / mutants

TT2_IMPORT(NtCreateSemaphore)
{
    Args a{ ctx };
    write_handle(a, 0, std::make_shared<Semaphore>(a.s32(2), a.s32(3)));
    a.ret(status::success);
}

TT2_IMPORT(NtReleaseSemaphore)
{
    Args a{ ctx };
    auto s = handle_as<Semaphore>(a.u32(0));
    if (!s)
        return a.ret(status::invalid_handle);
    int32_t previous = 0;
    bool over = false;
    with_lock([&] {
        previous = s->count;
        if (s->count + a.s32(1) > s->limit)
            over = true;
        else
            s->count += a.s32(1);
    });
    write_out<int32_t>(a, 2, previous);
    a.ret(over ? status::semaphore_limit_exceeded : status::success);
}

TT2_IMPORT(NtCreateMutant)
{
    Args a{ ctx };
    auto m = std::make_shared<Mutant>();
    if (a.u32(2))
    {
        m->owner = current_thread_id();
        m->recursion = 1;
    }
    write_handle(a, 0, m);
    a.ret(status::success);
}

TT2_IMPORT(NtReleaseMutant)
{
    Args a{ ctx };
    auto m = handle_as<Mutant>(a.u32(0));
    if (!m)
        return a.ret(status::invalid_handle);
    const uint32_t tid = current_thread_id();
    int32_t previous = 0;
    bool owned = true;
    with_lock([&] {
        if (m->owner != tid)
        {
            owned = false;
            return;
        }
        previous = m->recursion;
        if (--m->recursion == 0)
            m->owner = 0;
    });
    write_out<int32_t>(a, 1, previous);
    a.ret(owned ? status::success : status::mutant_not_owned);
}

// ---------------------------------------------------------------------- timers

TT2_IMPORT(NtCreateTimer)
{
    Args a{ ctx };
    // TIMER_TYPE: 0 = notification, 1 = synchronization
    write_handle(a, 0, std::make_shared<Timer>(a.u32(2) == 0));
    a.ret(status::success);
}

// NtSetTimerEx(handle, due_time*, apc_routine, mode, apc_context, resume, period_ms, previous_state*)
TT2_IMPORT(NtSetTimerEx)
{
    Args a{ ctx };
    auto t = handle_as<Timer>(a.u32(0));
    if (!t)
        return a.ret(status::invalid_handle);
    const auto deadline = deadline_from_nt(a.ptr<be<int64_t>>(1));
    with_lock([&] {
        t->signaled = false;
        t->armed = true;
        t->due = deadline.value_or(Clock::now());
        t->period = std::chrono::milliseconds(a.s32(6));
        t->apc_routine = a.u32(2);
        t->apc_context = a.u32(4);
    });
    if (t->apc_routine)
        log("kernel: timer APC routines are not delivered (%08X)", t->apc_routine);
    write_out<uint8_t>(a, 7, 0);
    a.ret(status::success);
}

TT2_IMPORT(NtCancelTimer)
{
    Args a{ ctx };
    auto t = handle_as<Timer>(a.u32(0));
    if (!t)
        return a.ret(status::invalid_handle);
    bool state = false;
    with_lock([&] {
        state = t->is_signaled(0);
        t->armed = false;
    });
    write_out<uint8_t>(a, 1, state ? 1 : 0);
    a.ret(status::success);
}

// ---------------------------------------------------------------------- waits

TT2_IMPORT(NtWaitForSingleObjectEx)
{
    Args a{ ctx };
    a.ret(wait_one(object_from_handle(a.u32(0)), a.ptr<be<int64_t>>(3), a.u32(2) != 0));
}

TT2_IMPORT(NtSignalAndWaitForSingleObjectEx)
{
    Args a{ ctx };
    auto signal = object_from_handle(a.u32(0));
    if (auto e = std::dynamic_pointer_cast<Event>(signal))
        set_event(*e);
    else if (auto s = std::dynamic_pointer_cast<Semaphore>(signal))
        with_lock([&] { s->count = std::min(s->count + 1, s->limit); });
    else if (auto m = std::dynamic_pointer_cast<Mutant>(signal))
        with_lock([&] {
            if (m->owner == current_thread_id() && --m->recursion == 0)
                m->owner = 0;
        });
    a.ret(wait_one(object_from_handle(a.u32(1)), a.ptr<be<int64_t>>(4), a.u32(3) != 0));
}

TT2_IMPORT(KeWaitForSingleObject)
{
    Args a{ ctx };
    a.ret(wait_one(object_from_pointer(a.u32(0)), a.ptr<be<int64_t>>(4), a.u32(3) != 0));
}

// KeWaitForMultipleObjects(count, objects*, wait_type, reason, mode, alertable, timeout*, wait_blocks*)
TT2_IMPORT(KeWaitForMultipleObjects)
{
    Args a{ ctx };
    const uint32_t count = a.u32(0);
    std::vector<std::shared_ptr<Object>> objects;
    for (uint32_t i = 0; i < count; i++)
        objects.push_back(object_from_pointer(load32(a.u32(1) + i * 4)));
    const bool wait_all = a.u32(2) == 0; // WaitAll = 0, WaitAny = 1
    a.ret(wait(objects, wait_all, deadline_from_nt(a.ptr<be<int64_t>>(6)), a.u32(5) != 0));
}

// ---------------------------------------------------------------------- handles / objects

TT2_IMPORT(NtClose)
{
    ctx.r3.u64 = close_handle(ctx.r3.u32) ? status::success : status::invalid_handle;
}

TT2_IMPORT(NtDuplicateObject)
{
    Args a{ ctx };
    const uint32_t h = duplicate_handle(a.u32(0));
    if (!h)
        return a.ret(status::invalid_handle);
    write_out<uint32_t>(a, 1, h);
    if (a.u32(2) & 1) // DUPLICATE_CLOSE_SOURCE
        close_handle(a.u32(0));
    a.ret(status::success);
}

TT2_IMPORT(ObReferenceObjectByHandle)
{
    Args a{ ctx };
    auto obj = object_from_handle(a.u32(0));
    if (!obj)
        return a.ret(status::invalid_handle);
    if (!obj->body)
        register_body(obj, 0x40);
    write_out<uint32_t>(a, 2, obj->body);
    a.ret(status::success);
}

TT2_IMPORT(ObReferenceObject) {}
TT2_IMPORT(ObDereferenceObject) {}

TT2_IMPORT(ObIsTitleObject) { ctx.r3.u64 = 1; }

// ---------------------------------------------------------------------- DPCs

// KDPC: +0x0C routine, +0x10 context, +0x14 arg1, +0x18 arg2
TT2_IMPORT(KeInitializeDpc)
{
    Args a{ ctx };
    const uint32_t dpc = a.u32(0);
    store16(dpc + 0, 19);
    store32(dpc + 0x0C, a.u32(1));
    store32(dpc + 0x10, a.u32(2));
    store32(dpc + 0x14, 0);
    store32(dpc + 0x18, 0);
}

TT2_IMPORT(KeInsertQueueDpc)
{
    Args a{ ctx };
    const uint32_t dpc = a.u32(0);
    store32(dpc + 0x14, a.u32(1));
    store32(dpc + 0x18, a.u32(2));
    // Run it now on the caller's thread; DPCs here come from the graphics interrupt path.
    call_guest(load32(dpc + 0x0C), dpc, load32(dpc + 0x10), a.u32(1), a.u32(2));
    a.ret(1);
}

// ---------------------------------------------------------------------- spinlocks

TT2_IMPORT(KfAcquireSpinLock)
{
    const uint32_t lock = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    int spins = 0;
    while (!cas32(lock, 0, me))
        backoff(spins);
    ctx.r3.u64 = 0; // previous IRQL
}

TT2_IMPORT(KfReleaseSpinLock)
{
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + ctx.r3.u32), 0u, __ATOMIC_RELEASE);
}

TT2_IMPORT(KeAcquireSpinLockAtRaisedIrql)
{
    const uint32_t lock = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    int spins = 0;
    while (!cas32(lock, 0, me))
        backoff(spins);
}

TT2_IMPORT(KeReleaseSpinLockFromRaisedIrql)
{
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + ctx.r3.u32), 0u, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------- critical sections
// RTL_CRITICAL_SECTION: 16-byte dispatcher header, +0x10 LockCount, +0x14 RecursionCount,
// +0x18 OwningThread.

TT2_IMPORT(RtlInitializeCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    std::memset(g_base + cs, 0, 0x1C);
    g_base[cs] = 1; // synchronization event type in the header
    store32(cs + 0x10, 0xFFFFFFFF);
}

TT2_IMPORT(RtlInitializeCriticalSectionAndSpinCount)
{
    const uint32_t cs = ctx.r3.u32;
    std::memset(g_base + cs, 0, 0x1C);
    g_base[cs] = 1;
    store32(cs + 0x10, 0xFFFFFFFF);
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(RtlEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    if (load32(cs + 0x18) == me)
    {
        store32(cs + 0x14, load32(cs + 0x14) + 1);
        return;
    }
    int spins = 0;
    while (!cas32(cs + 0x18, 0, me))
        backoff(spins);
    store32(cs + 0x14, 1);
    store32(cs + 0x10, load32(cs + 0x10) + 1);
}

TT2_IMPORT(RtlTryEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    if (load32(cs + 0x18) == me)
    {
        store32(cs + 0x14, load32(cs + 0x14) + 1);
        ctx.r3.u64 = 1;
        return;
    }
    if (cas32(cs + 0x18, 0, me))
    {
        store32(cs + 0x14, 1);
        store32(cs + 0x10, load32(cs + 0x10) + 1);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

TT2_IMPORT(RtlLeaveCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t recursion = load32(cs + 0x14);
    if (recursion > 1)
    {
        store32(cs + 0x14, recursion - 1);
        return;
    }
    store32(cs + 0x14, 0);
    store32(cs + 0x10, load32(cs + 0x10) - 1);
    __atomic_store_n(reinterpret_cast<uint32_t*>(g_base + cs + 0x18), 0u, __ATOMIC_RELEASE);
}
