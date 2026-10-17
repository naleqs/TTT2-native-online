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

        // Frame-locked time for rollback re-simulation: when set, the guest sees this value.
        std::atomic<uint64_t> g_override{ 0 };

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
                    const uint64_t tb = host_timebase();
                    const uint64_t interrupt_time = tb / 5; // 100 ns units
                    FILETIME ft;
                    GetSystemTimePreciseAsFileTime(&ft);
                    store64(b, interrupt_time);
                    store64(b + 8, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
                    store32(b + 16, uint32_t(tb / 50'000));
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

    void timebase_override(uint64_t value) { g_override = value; }
}

extern "C" uint64_t tt2_guest_timebase()
{
    const uint64_t o = tt2::g_override.load(std::memory_order_relaxed);
    return o ? o : tt2::host_timebase();
}
