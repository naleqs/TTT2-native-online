#pragma once

#include "objects.h"

#include <atomic>
#include <thread>

namespace tt2::kernel
{
    struct Thread final : Object
    {
        uint32_t id = 0;
        uint32_t pcr = 0;            // KPCR (r13), followed by the TLS slots and the TEB
        uint32_t stack_base = 0;     // lowest address of the guest stack
        uint32_t stack_size = 0;
        uint32_t static_tls = 0;     // copy of the image's __declspec(thread) data
        uint32_t start_address = 0, start_context = 0, xapi_startup = 0;
        bool exited = false;
        uint32_t exit_code = 0;
        int suspend_count = 0;
        std::thread host;
        void* native = nullptr;      // Win32 handle of the host thread (for NtSuspendThread)
        bool started = false;        // running guest code (a suspended start has not begun)
        bool frozen = false;         // suspended by another thread with Win32 SuspendThread
        PPCContext* context = nullptr; // live registers (read racily by the thread dump)
        const char* const* current_import = nullptr; // this thread's t_current_import
        std::string name;
        bool adopted = false;            // a runtime host thread running guest callbacks
        std::atomic<int> blocked{ 0 };   // inside a kernel wait or sleep (netplay barrier)
        int busy_frames = 0;             // netplay barrier: frames it kept the barrier waiting
        int idle_misses = 0;             // wait_for_all_threads: times it did not settle
        std::atomic<int64_t> last_io_ms{ -1000000 }; // last file call (steady clock, ms): a loader

        ObjectType type() const override { return ObjectType::Thread; }
        bool is_signaled(uint32_t) const override { return exited; }
        ~Thread() override;
    };

    // Starts a guest thread. With `suspended`, it waits for NtResumeThread.
    std::shared_ptr<Thread> start_thread(uint32_t start_address, uint32_t start_context, uint32_t xapi_startup,
                                         uint32_t stack_size, bool suspended, const char* name);

    // Gives the calling host thread a guest identity (PCR, TLS, stack) so it can call guest
    // code: used for runtime threads that run guest callbacks (interrupts, audio).
    std::shared_ptr<Thread> adopt_host_thread(const char* name);

    std::shared_ptr<Thread> current_thread();

    // Debugging: logs every guest thread's link register and guest backtrace.
    void dump_threads();
    // Guest function whose recompiled code contains the host address (0: not game code).
    uint32_t guest_function_at(uintptr_t rip);
    void for_each_thread(const std::function<void(Thread&)>& fn);

    // Marks the calling guest thread as blocked for the lifetime of the object (kernel waits,
    // sleeps, lock contention). The netplay barrier waits until no other thread is running.
    struct BlockedScope
    {
        BlockedScope();
        ~BlockedScope();
        Thread* thread;
    };

    // Netplay: waits (up to `timeout_ms`) until every other guest thread is blocked, so work
    // other threads do between two frames (loading) completes before the next frame on every PC.
    // Threads that never block (spinning) stop being waited for after a few frames.
    void wait_for_other_threads(int timeout_ms);

    // Waits (up to `timeout_ms`) until every guest thread started at `start_address` is blocked.
    // False on timeout.
    bool wait_until_blocked(uint32_t start_address, int timeout_ms);
    // Waits (up to `timeout_ms`) until every other guest thread is blocked (threads that never
    // block are given up on after a few frames, as in wait_for_other_threads).
    void wait_for_all_threads(int timeout_ms);

    // File calls mark the calling thread as a loader for a few seconds (the barrier's scope).
    void note_file_activity();
    extern std::atomic<uint64_t> g_file_reads; // NtReadFile calls so far (diagnostics)

    // Thrown by ExTerminateThread and caught at the top of the thread.
    struct ThreadExit
    {
        uint32_t code;
    };
}
