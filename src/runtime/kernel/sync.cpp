// Events, semaphores, mutants, timers, waits, DPCs, spinlocks and critical sections.

#include "objects.h"
#include "threads.h"
#include "../memory.h"

#include <immintrin.h>
#include <condition_variable>
#include <thread>
#include <unordered_map>

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
    // Releases the threads waiting right now (they see the pulse count change), then the
    // event is unsignaled.
    uint32_t previous = 0;
    with_lock([&] {
        previous = e->signaled;
        e->pulses++;
        e->signaled = false;
        e->mirror_signal_state(0);
    });
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
    // (signal, wait, alertable, ?, timeout*). The signal happens under the wait lock, so the
    // pair is atomic as on the console: a thread woken by the signal (e.g. one taking a
    // released mutex and pulsing an event) cannot run before this thread is waiting.
    auto signal = object_from_handle(a.u32(0));
    auto target = object_from_handle(a.u32(1));
    if (!target)
        return a.ret(status::invalid_handle);
    const uint32_t me = current_thread_id();
    auto before = [&] {
        if (auto e = std::dynamic_pointer_cast<Event>(signal))
        {
            e->signaled = true;
            e->mirror_signal_state(1);
        }
        else if (auto s = std::dynamic_pointer_cast<Semaphore>(signal))
            s->count = std::min(s->count + 1, s->limit);
        else if (auto m = std::dynamic_pointer_cast<Mutant>(signal))
        {
            if (m->owner == me && --m->recursion == 0)
                m->owner = 0;
        }
    };
    a.ret(wait({ target }, false, deadline_from_nt(a.ptr<be<int64_t>>(4)), a.u32(2) != 0, before));
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

// ---------------------------------------------------------------------- spinlocks and critical sections
//
// Lock state lives on the host, keyed by the guest address of the lock, not in the guest
// structure: rollback rewinds guest memory, and a rewound "owner" field would resurrect a lock
// that was released long ago (deadlocking whoever waits for it). Guest code only reaches these
// locks through the kernel calls below. The guest fields are still updated for code that reads
// them (debug checks).

namespace
{
    struct HostLock
    {
        uint32_t owner = 0;     // KTHREAD pointer of the owner, 0 = free
        int32_t recursion = 0;
    };

    std::mutex g_locks_mutex;
    std::condition_variable g_locks_cv;
    std::unordered_map<uint32_t, HostLock> g_locks; // node-based: references stay valid

    void acquire(uint32_t address, uint32_t me, bool recursive)
    {
        std::unique_lock lock(g_locks_mutex);
        HostLock& l = g_locks[address];
        if (recursive && l.owner == me)
        {
            l.recursion++;
            return;
        }
        BlockedScope blocked;
        g_locks_cv.wait(lock, [&] { return l.owner == 0; });
        l.owner = me;
        l.recursion = 1;
    }

    bool try_acquire(uint32_t address, uint32_t me)
    {
        std::lock_guard lock(g_locks_mutex);
        HostLock& l = g_locks[address];
        if (l.owner == me)
        {
            l.recursion++;
            return true;
        }
        if (l.owner != 0)
            return false;
        l.owner = me;
        l.recursion = 1;
        return true;
    }

    void mirror_cs(uint32_t cs, uint32_t owner, int32_t recursion);

    // Returns the remaining recursion count. For a critical section (`cs`), its guest-visible
    // fields are updated while the lock is still held: once another thread can take the lock,
    // the critical section may already be gone (a stack object whose owner returned), and a late
    // write would land in someone else's stack frame.
    int32_t release(uint32_t address, bool cs = false)
    {
        int32_t remaining = 0;
        {
            std::lock_guard lock(g_locks_mutex);
            HostLock& l = g_locks[address];
            if (l.recursion > 0)
                l.recursion--;
            remaining = l.recursion;
            if (cs)
                mirror_cs(address, remaining ? l.owner : 0, remaining);
            if (remaining == 0)
                l.owner = 0;
        }
        if (remaining == 0)
            g_locks_cv.notify_all();
        return remaining;
    }

    void reset(uint32_t address)
    {
        std::lock_guard lock(g_locks_mutex);
        g_locks[address] = HostLock{};
    }

    // RTL_CRITICAL_SECTION: 16-byte dispatcher header, +0x10 LockCount, +0x14 RecursionCount,
    // +0x18 OwningThread.
    void mirror_cs(uint32_t cs, uint32_t owner, int32_t recursion)
    {
        store32(cs + 0x10, recursion ? 0 : 0xFFFFFFFF);
        store32(cs + 0x14, uint32_t(recursion));
        store32(cs + 0x18, owner);
    }
}

namespace tt2::kernel
{
    // Debugging: every held lock and its owner (KTHREAD pointer).
    void dump_locks()
    {
        std::vector<std::pair<uint32_t, HostLock>> held;
        {
            std::lock_guard lock(g_locks_mutex);
            for (auto& [address, l] : g_locks)
                if (l.owner)
                    held.push_back({ address, l });
        }
        for (auto& [address, l] : held)
            log("  lock %08X held by KTHREAD %08X (recursion %d)", address, l.owner, l.recursion);
    }
}

TT2_IMPORT(KfAcquireSpinLock)
{
    acquire(ctx.r3.u32, owner_id(ctx), false);
    ctx.r3.u64 = 0; // previous IRQL
}

TT2_IMPORT(KfReleaseSpinLock) { release(ctx.r3.u32); }

TT2_IMPORT(KeAcquireSpinLockAtRaisedIrql) { acquire(ctx.r3.u32, owner_id(ctx), false); }

TT2_IMPORT(KeReleaseSpinLockFromRaisedIrql) { release(ctx.r3.u32); }

TT2_IMPORT(RtlInitializeCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    std::memset(g_base + cs, 0, 0x1C);
    g_base[cs] = 1; // synchronization event type in the header
    store32(cs + 0x10, 0xFFFFFFFF);
    reset(cs);
}

TT2_IMPORT(RtlInitializeCriticalSectionAndSpinCount)
{
    const uint32_t cs = ctx.r3.u32;
    std::memset(g_base + cs, 0, 0x1C);
    g_base[cs] = 1;
    store32(cs + 0x10, 0xFFFFFFFF);
    reset(cs);
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(RtlEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    acquire(cs, me, true);
    mirror_cs(cs, me, int32_t(load32(cs + 0x14)) + 1);
}

TT2_IMPORT(RtlTryEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    const uint32_t me = owner_id(ctx);
    if (try_acquire(cs, me))
    {
        mirror_cs(cs, me, int32_t(load32(cs + 0x14)) + 1);
        ctx.r3.u64 = 1;
    }
    else
        ctx.r3.u64 = 0;
}

TT2_IMPORT(RtlLeaveCriticalSection)
{
    release(ctx.r3.u32, true);
}
