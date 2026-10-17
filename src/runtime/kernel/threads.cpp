#include "threads.h"
#include "../image.h"
#include "../memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bitset>

namespace tt2
{
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

            // ObReferenceObjectByHandle hands out the TEB as the thread object.
            t.body = teb;
        }

        void run_guest_thread(std::shared_ptr<Thread> self)
        {
            t_thread = self;
            {
                std::unique_lock lock(wait_mutex());
                // Suspended threads start on NtResumeThread.
                while (self->suspend_count > 0)
                {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    lock.lock();
                }
            }

            PPCContext ctx{};
            ctx.r1.u64 = self->stack_base + self->stack_size - 0x100;
            ctx.r13.u64 = self->pcr;
            ctx.fpscr.loadFromHost();
            set_current_context(&ctx);

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
        t->suspend_count = suspended ? 1 : 0;
        t->name = name ? name : "";
        register_body(t, 0);
        t->host = std::thread(run_guest_thread, t);
        return t;
    }

    std::shared_ptr<Thread> adopt_host_thread(const char* name)
    {
        auto t = std::make_shared<Thread>();
        setup_guest_identity(*t, 64 * 1024);
        t->name = name;
        register_body(t, 0);
        t_thread = t;

        auto* ctx = new PPCContext{};
        ctx->r1.u64 = t->stack_base + t->stack_size - 0x100;
        ctx->r13.u64 = t->pcr;
        ctx->fpscr.loadFromHost();
        set_current_context(ctx);
        return t;
    }

    std::shared_ptr<Thread> current_thread() { return t_thread; }

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
    with_lock([&] {
        previous = t->suspend_count;
        if (t->suspend_count > 0)
            t->suspend_count--;
    });
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
    // A running host thread cannot be frozen safely from outside; only threads that have not
    // started yet honour this. Logged so it shows up if the game relies on it.
    int previous = 0;
    with_lock([&] { previous = t->suspend_count++; });
    if (previous == 0 && t.get() != current_thread().get())
        log("kernel: NtSuspendThread on running thread %u is not enforced", t->id);
    if (auto* p = a.ptr<be<uint32_t>>(1))
        *p = uint32_t(previous);
    a.ret(status::success);
}

TT2_IMPORT(KeDelayExecutionThread)
{
    Args a{ ctx };
    const auto* interval = a.ptr<be<int64_t>>(2);
    const auto deadline = deadline_from_nt(interval);
    if (!deadline || *deadline <= Clock::now())
        std::this_thread::yield();
    else
        std::this_thread::sleep_until(*deadline);
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
