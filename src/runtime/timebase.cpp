#include "guest.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace tt2
{
    uint32_t timestamp_bundle(); // image.cpp

    namespace
    {
        LARGE_INTEGER g_frequency{}, g_start{};
        std::atomic<bool> g_running{ false };
        std::thread g_thread;

        // Rollback: the simulation thread sees frame-locked time (frame base + a small step per
        // read) so a re-simulated frame reads exactly the times the original did. Other
        // threads keep the real clock (as in Tekken6-rollback's virtual timebase).
        thread_local bool t_virtual = false;
        thread_local uint64_t t_reads = 0;
        thread_local uint64_t t_base_seen = 0;
        std::atomic<uint64_t> g_frame_base{ 0 };

        // Netplay: every clock of every thread follows the frame-locked time, from a fixed start,
        // so both games read the same times (random seeds, timers) whatever the PCs' clocks say.
        std::atomic<bool> g_deterministic{ false };
        constexpr uint64_t kDeterministicStart = 50'000'000ull * 1000; // 1000 s of uptime
        constexpr uint64_t kDeterministicEpoch = 133'800'000'000'000'000ull; // 2025 in 100 ns since 1601

        uint64_t host_timebase()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const uint64_t ticks = uint64_t(now.QuadPart - g_start.QuadPart);
            // 50 MHz without overflow: split into whole seconds and remainder.
            const uint64_t f = uint64_t(g_frequency.QuadPart);
            return (ticks / f) * 50'000'000ull + (ticks % f) * 50'000'000ull / f;
        }

        // KeTimeStampBundle: be64 InterruptTime, be64 SystemTime, be32 TickCount (ms)
        void bundle_thread()
        {
            while (g_running)
            {
                if (const uint32_t b = timestamp_bundle())
                {
                    if (g_deterministic)
                    {
                        const uint64_t tb = g_frame_base.load(std::memory_order_relaxed);
                        store64(b, tb / 5);
                        store64(b + 8, kDeterministicEpoch + tb / 5);
                        store32(b + 16, uint32_t(tb / 50'000));
                    }
                    else
                    {
                        const uint64_t tb = host_timebase();
                        const uint64_t interrupt_time = tb / 5; // 100 ns units
                        FILETIME ft;
                        GetSystemTimePreciseAsFileTime(&ft);
                        store64(b, interrupt_time);
                        store64(b + 8, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
                        store32(b + 16, uint32_t(tb / 50'000));
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void timebase_start()
    {
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_start);
        timeBeginPeriod(1);
        if (!g_running.exchange(true))
            g_thread = std::thread(bundle_thread);
    }

    void timebase_stop()
    {
        if (g_running.exchange(false) && g_thread.joinable())
            g_thread.join();
        timeEndPeriod(1);
    }

    uint64_t timebase_now() { return host_timebase(); }

    void timebase_set_deterministic()
    {
        g_frame_base = kDeterministicStart;
        g_deterministic = true;
    }

    bool timebase_deterministic() { return g_deterministic.load(std::memory_order_relaxed); }

    uint64_t timebase_deterministic_start() { return kDeterministicStart; }

    // KeQuerySystemTime and friends in deterministic mode (100 ns units since 1601).
    uint64_t timebase_deterministic_filetime() { return kDeterministicEpoch + g_frame_base.load(std::memory_order_relaxed) / 5; }

    // Called by the simulation thread at each frame boundary (rollback active).
    void timebase_frame(uint64_t frame_base)
    {
        t_virtual = true;
        t_reads = 0;
        g_frame_base = frame_base;
    }
}

extern "C" uint64_t tt2_guest_timebase()
{
    if (tt2::t_virtual)
        return tt2::g_frame_base.load(std::memory_order_relaxed) + (++tt2::t_reads) * 50;
    if (tt2::g_deterministic.load(std::memory_order_relaxed))
    {
        // Other threads: the current frame's time, plus a small step per read so loops waiting
        // for time to pass still see it move.
        const uint64_t base = tt2::g_frame_base.load(std::memory_order_relaxed);
        if (tt2::t_base_seen != base)
        {
            tt2::t_base_seen = base;
            tt2::t_reads = 0;
        }
        return base + (++tt2::t_reads) * 50;
    }
    return tt2::host_timebase();
}
