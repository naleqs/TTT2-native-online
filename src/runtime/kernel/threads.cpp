#include "threads.h"
#include "../image.h"
#include "../memory.h"
#include "../rollback/game_rollback.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bitset>

namespace tt2
{
    thread_local const char* t_current_import = nullptr;

    namespace
    {
        thread_local PPCContext* t_context = nullptr;
    }

    PPCContext* current_context() { return t_context; }
    void set_current_context(PPCContext* ctx) { t_context = ctx; }
}

namespace tt2::kernel
{
    namespace
    {
        // Per-thread kernel block, as the game sees it through r13:
        //   PCR + 0x000  TLS data pointer (static __declspec(thread) data)
        //   PCR + 0x100  current thread (KTHREAD / TEB) pointer
        //   PCR + 0x10C  current processor number
        //   TEB + 0x14C  thread id, TEB + 0x160 last Win32 error
        // Layout as used by Unleashed Recompiled (guest_thread.cpp).
        constexpr uint32_t kPcrSize = 0xAB0;
        constexpr uint32_t kTlsSlotsSize = 0x100;
        constexpr uint32_t kTebSize = 0x2E0;
        constexpr uint32_t kBlockSize = kPcrSize + kTlsSlotsSize + kTebSize;

        thread_local std::shared_ptr<Thread> t_thread;
        std::mutex g_registry_mutex;
        std::vector<std::weak_ptr<Thread>> g_registry;

        void register_thread(const std::shared_ptr<Thread>& t)
        {
            std::lock_guard lock(g_registry_mutex);
            g_registry.push_back(t);
        }
        thread_local uint32_t t_tls[64]{};
        std::atomic<uint32_t> g_next_thread_id{ 1 };

        std::mutex g_tls_mutex;
        std::bitset<64> g_tls_used;

        void setup_guest_identity(Thread& t, uint32_t stack_size)
        {
            t.id = g_next_thread_id++;
            t.pcr = g_memory.system_alloc(kBlockSize, 0x1000);
            const uint32_t teb = t.pcr + kPcrSize + kTlsSlotsSize;

            if (g_image.tls_data_size)
            {
                t.static_tls = g_memory.system_alloc(g_image.tls_data_size, 16);
                if (g_image.tls_raw_address && g_image.tls_raw_size)
                    std::memcpy(g_base + t.static_tls, g_base + g_image.tls_raw_address, std::min(g_image.tls_raw_size, g_image.tls_data_size));
            }
            store32(t.pcr + 0x000, t.static_tls ? t.static_tls : t.pcr + kPcrSize);
            store32(t.pcr + 0x100, teb);
            g_base[t.pcr + 0x10C] = 0;
            store32(teb + 0x14C, t.id);

            t.stack_size = std::max<uint32_t>(stack_size ? stack_size : g_image.stack_size, 64 * 1024);
            t.stack_size = (t.stack_size + 0xFFFF) & ~0xFFFFu;
            t.stack_base = g_memory.virt.alloc(t.stack_size, 0x10000, true);
            if (!t.stack_base)
                fatal("out of guest memory for a %u KB thread stack", t.stack_size / 1024);
            std::memset(g_base + t.stack_base, 0, t.stack_size);
            if (std::getenv("TT2_LOG_STACKS"))
                tt2::log("thread %u stack %08X-%08X", t.id, t.stack_base, t.stack_base + t.stack_size);

            // ObReferenceObjectByHandle hands out the TEB as the thread object.
            t.body = teb;
        }

        void run_guest_thread(std::shared_ptr<Thread> self)
        {
            t_thread = self;
            HANDLE native = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &native, 0, FALSE, DUPLICATE_SAME_ACCESS);
            self->native = native;
            // Threads created suspended start on NtResumeThread.
            block_until([&] { return self->suspend_count <= 0; });
            with_lock([&] { self->started = true; });

            PPCContext ctx{};
            ctx.r1.u64 = self->stack_base + self->stack_size - 0x100;
            ctx.r13.u64 = self->pcr;
            ctx.fpscr.loadFromHost();
            set_current_context(&ctx);
            self->context = &ctx;
            self->current_import = &t_current_import;

            uint32_t code = 0;
            try
            {
                if (self->xapi_startup)
                {
                    ctx.r3.u64 = self->start_address;
                    ctx.r4.u64 = self->start_context;
                    find_function(self->xapi_startup)(ctx, g_base);
                }
                else
                {
                    ctx.r3.u64 = self->start_context;
                    find_function(self->start_address)(ctx, g_base);
                }
                code = ctx.r3.u32;
            }
            catch (const ThreadExit& e)
            {
                code = e.code;
            }

            with_lock([&] {
                self->exited = true;
                self->exit_code = code;
            });
            self->context = nullptr; // the PPCContext lives on this host stack
            set_current_context(nullptr);
        }
    }

    Thread::~Thread()
    {
        if (host.joinable())
            host.detach();
    }

    std::shared_ptr<Thread> start_thread(uint32_t start_address, uint32_t start_context, uint32_t xapi_startup,
                                         uint32_t stack_size, bool suspended, const char* name)
    {
        auto t = std::make_shared<Thread>();
        setup_guest_identity(*t, stack_size);
        t->start_address = start_address;
        t->start_context = start_context;
        t->xapi_startup = xapi_startup;
        t->routine = game::thread_routine(start_address, start_context);
        t->suspend_count = suspended ? 1 : 0;
        t->name = name ? name : "";
        register_body(t, 0);
        register_thread(t);
        t->host = std::thread(run_guest_thread, t);
        return t;
    }

    std::shared_ptr<Thread> adopt_host_thread(const char* name)
    {
        auto t = std::make_shared<Thread>();
        setup_guest_identity(*t, 64 * 1024);
        t->name = name;
        t->adopted = true;
        register_body(t, 0);
        register_thread(t);
        t_thread = t;

        auto* ctx = new PPCContext{};
        ctx->r1.u64 = t->stack_base + t->stack_size - 0x100;
        ctx->r13.u64 = t->pcr;
        ctx->fpscr.loadFromHost();
        set_current_context(ctx);
        t->context = ctx;
        t->current_import = &t_current_import;
        return t;
    }

    std::shared_ptr<Thread> current_thread() { return t_thread; }

    BlockedScope::BlockedScope() : thread(t_thread.get())
    {
        if (thread)
            thread->blocked.fetch_add(1, std::memory_order_acq_rel);
    }

    BlockedScope::~BlockedScope()
    {
        if (thread)
            thread->blocked.fetch_sub(1, std::memory_order_acq_rel);
    }

    int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void note_file_activity()
    {
        if (Thread* t = t_thread.get())
            t->last_io_ms.store(now_ms(), std::memory_order_relaxed);
    }

    void wait_for_all_threads(int timeout_ms)
    {
        Thread* self = t_thread.get();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int quiet = 0;
        for (;;)
        {
            std::vector<Thread*> running;
            for_each_thread([&](Thread& t) {
                if (&t == self || t.adopted || !t.started || t.exited || t.suspend_count > 0 || t.idle_misses >= 3)
                    return;
                if (t.blocked.load(std::memory_order_acquire) == 0)
                    running.push_back(&t);
            });
            if (running.empty())
            {
                if (++quiet >= 2)
                    return;
            }
            else
                quiet = 0;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (Thread* t : running)
                    if (++t->idle_misses == 3)
                        tt2::log("netplay: thread %u never settles; no longer waited for", t->id);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    bool wait_until_blocked(uint32_t start_address, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;)
        {
            bool busy = false;
            for_each_thread([&](Thread& t) {
                if ((t.start_address == start_address || t.routine == start_address) && t.started && !t.exited &&
                    t.blocked.load(std::memory_order_acquire) == 0 && !(t.context && game::thread_waiting_in_guest(t.context->r1.u32)))
                    busy = true;
            });
            if (!busy)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                static int logged = 0;
                if (logged++ < 3)
                    for_each_thread([&](Thread& t) {
                        if ((t.start_address == start_address || t.routine == start_address) && t.started && !t.exited &&
                            t.blocked.load(std::memory_order_acquire) == 0)
                            log("threads: t%u (%08X) is not blocked: frozen %d, suspend count %d, in %s, lr %08X", t.id, start_address,
                                t.frozen ? 1 : 0, t.suspend_count, t.current_import && *t.current_import ? *t.current_import : "guest code",
                                t.context ? uint32_t(t.context->lr) : 0);
                    });
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void wait_for_other_threads(int timeout_ms)
    {
        Thread* self = t_thread.get();
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(timeout_ms);
        int quiet = 0;
        for (;;)
        {
            std::vector<Thread*> running;
            for_each_thread([&](Thread& t) {
                if (&t == self || t.adopted || !t.started || t.exited || t.suspend_count > 0 || t.busy_frames >= 3)
                    return;
                // Only loaders: threads that used the file system in the last 3 seconds. Waiting
                // for every worker (physics, audio) would cost a lot of time every frame.
                if (now_ms() - t.last_io_ms.load(std::memory_order_relaxed) > 3000)
                    return;
                if (t.blocked.load(std::memory_order_acquire) == 0)
                    running.push_back(&t);
            });
            if (running.empty())
            {
                // Two quiet looks in a row: a thread woken in between has had time to show up.
                if (++quiet >= 2)
                    return;
            }
            else
                quiet = 0;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (Thread* t : running)
                    if (++t->busy_frames == 3)
                        tt2::log("netplay: thread %u (%s) never goes idle; no longer waited for", t->id, t->name.c_str());
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void for_each_thread(const std::function<void(Thread&)>& fn)
    {
        std::lock_guard lock(g_registry_mutex);
        for (auto& w : g_registry)
            if (auto t = w.lock())
                fn(*t);
    }

    // Guest function whose recompiled host code contains `rip` (nearest host start below it).
    uint32_t guest_function_at(uintptr_t rip)
    {
        static std::vector<std::pair<uintptr_t, uint32_t>> table; // host start -> guest address
        if (table.empty())
        {
            for (size_t i = 0; PPCFuncMappings[i].guest; i++)
                if (PPCFuncMappings[i].host)
                    table.push_back({ reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), uint32_t(PPCFuncMappings[i].guest) });
            std::sort(table.begin(), table.end());
        }
        if (table.empty() || rip < table.front().first || rip > table.back().first + 0x100000)
            return 0; // not recompiled game code (runtime or system code)
        auto it = std::upper_bound(table.begin(), table.end(), std::make_pair(rip, UINT32_MAX));
        return it == table.begin() ? 0 : std::prev(it)->second;
    }

    void dump_locks(); // sync.cpp

    void dump_threads()
    {
        std::lock_guard lock(g_registry_mutex);
        log("---- guest threads ----");
        // Where each host thread is executing right now, as a guest function. Nothing may be
        // logged (or allocated) while a thread is suspended: it could hold that lock.
        std::vector<std::pair<uint32_t, uintptr_t>> positions;
        positions.reserve(g_registry.size());
        for (auto& w : g_registry)
        {
            auto t = w.lock();
            if (!t || !t->native || t->exited || t.get() == current_thread().get())
                continue;
            if (SuspendThread(t->native) == DWORD(-1))
                continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            const bool ok = GetThreadContext(t->native, &c);
            ResumeThread(t->native);
            if (ok)
                positions.push_back({ t->id, uintptr_t(c.Rip) });
        }
        for (auto [id, rip] : positions)
            if (const uint32_t fn = guest_function_at(rip))
                log("  t%u is running game code in sub_%08X", id, fn);
        for (auto& w : g_registry)
        {
            auto t = w.lock();
            if (!t || !t->context || t->exited)
                continue;
            const PPCContext* c = t->context;
            const char* import = t->current_import ? *t->current_import : nullptr;
            log("  t%u %-22s KTHREAD %08X %s%s lr=%08llX r3=%08X r1=%08X%s", t->id, t->name.c_str(), load32(t->pcr + 0x100), import ? "in " : "",
                import ? import : "", (unsigned long long)c->lr, c->r3.u32, c->r1.u32, guest_backtrace(c->r1.u32, 14).c_str());
        }
        dump_locks();
    }

    std::shared_ptr<Object> current_thread_object() { return t_thread; }

    uint32_t current_thread_id() { return t_thread ? t_thread->id : 0; }
}

using namespace tt2;
using namespace tt2::kernel;

// ExCreateThread(PHANDLE handle, DWORD stack_size, PDWORD thread_id, xapi_startup,
//                start_address, start_context, DWORD creation_flags)
TT2_IMPORT(ExCreateThread)
{
    Args a{ ctx };
    const uint32_t flags = a.u32(6);
    auto t = start_thread(a.u32(4), a.u32(5), a.u32(3), a.u32(1), (flags & 1) != 0, nullptr);
    if (auto* h = a.ptr<be<uint32_t>>(0))
        *h = create_handle(t);
    if (auto* id = a.ptr<be<uint32_t>>(2))
        *id = t->id;
    a.ret(status::success);
}

TT2_IMPORT(ExTerminateThread)
{
    throw ThreadExit{ ctx.r3.u32 };
}

TT2_IMPORT(NtResumeThread)
{
    Args a{ ctx };
    auto t = handle_as<Thread>(a.u32(0));
    if (!t)
        return a.ret(status::invalid_handle);
    int previous = 0;
    bool resume_host = false;
    with_lock([&] {
        previous = t->suspend_count;
        if (t->suspend_count > 0)
            t->suspend_count--;
        // A running thread suspended by another thread was frozen with Win32 SuspendThread.
        resume_host = t->suspend_count == 0 && previous > 0 && t->started && t->frozen;
        if (resume_host)
            t->frozen = false;
    });
    if (resume_host && t->native)
        ResumeThread(t->native);
    if (auto* p = a.ptr<be<uint32_t>>(1))
        *p = uint32_t(previous);
    a.ret(status::success);
}

TT2_IMPORT(NtSuspendThread)
{
    Args a{ ctx };
    auto t = handle_as<Thread>(a.u32(0));
    if (!t)
        return a.ret(status::invalid_handle);
    int previous = 0;
    bool freeze_host = false;
    const bool self = t.get() == current_thread().get();
    with_lock([&] {
        previous = t->suspend_count++;
        freeze_host = !self && previous == 0 && t->started && !t->exited;
        if (freeze_host)
            t->frozen = true;
    });
    if (auto* p = a.ptr<be<uint32_t>>(1))
        *p = uint32_t(previous);
    a.ret(status::success);

    if (self)
    {
        // Suspending yourself blocks until another thread resumes you (the XAPI idiom for
        // "sleep until woken"). Returning at once would turn it into a busy loop.
        block_until([&] { return t->suspend_count <= 0; });
    }
    else if (freeze_host && t->native)
    {
        SuspendThread(t->native);
    }
}

TT2_IMPORT(KeDelayExecutionThread)
{
    Args a{ ctx };
    const auto* interval = a.ptr<be<int64_t>>(2);
    const auto deadline = deadline_from_nt(interval);
    if (!deadline || *deadline <= Clock::now())
        std::this_thread::yield();
    else
    {
        BlockedScope blocked;
        std::this_thread::sleep_until(*deadline);
    }
    a.ret(status::success);
}

TT2_IMPORT(NtYieldExecution)
{
    std::this_thread::yield();
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(KeSetAffinityThread)
{
    Args a{ ctx };
    if (auto* prev = a.ptr<be<uint32_t>>(2))
        *prev = 0xFF;
    a.ret(status::success);
}

TT2_IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }

TT2_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }

namespace
{
    uint32_t g_process_type = 1; // user title
}

TT2_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = g_process_type; }

TT2_IMPORT(KeSetCurrentProcessType) { g_process_type = ctx.r3.u32; }

TT2_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(g_tls_mutex);
    for (uint32_t i = 0; i < 64; i++)
    {
        if (!g_tls_used[i])
        {
            g_tls_used[i] = true;
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

TT2_IMPORT(KeTlsFree)
{
    std::lock_guard lock(g_tls_mutex);
    const uint32_t i = ctx.r3.u32;
    if (i < 64)
        g_tls_used[i] = false;
    ctx.r3.u64 = 1;
}

TT2_IMPORT(KeTlsGetValue)
{
    const uint32_t i = ctx.r3.u32;
    ctx.r3.u64 = i < 64 ? t_tls[i] : 0;
}

TT2_IMPORT(KeTlsSetValue)
{
    const uint32_t i = ctx.r3.u32;
    if (i < 64)
        t_tls[i] = ctx.r4.u32;
    ctx.r3.u64 = i < 64 ? 1 : 0;
}

TT2_IMPORT(NtQueueApcThread)
{
    log("kernel: NtQueueApcThread ignored");
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(KeEnterCriticalRegion) {}
TT2_IMPORT(KeLeaveCriticalRegion) {}
