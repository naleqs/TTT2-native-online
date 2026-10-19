// SnapshotRing test: random writes into tracked memory, save every frame, then load random
// earlier frames and compare against full copies taken at each frame. A second thread writes
// to its own area concurrently: those pages must become foreign and never roll back.

#include "rollback/snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <thread>
#include <random>
#include <vector>

using tt2::rollback::SnapshotRing;

namespace
{
    SnapshotRing* g_ring = nullptr;
    thread_local bool t_simulation = false;

    LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0] == 1 && g_ring &&
            g_ring->on_write_fault(info->ExceptionRecord->ExceptionInformation[1], t_simulation))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

int main()
{
    constexpr size_t kSize = 64ull << 20;   // reservation (stands in for the 4 GB guest space)
    constexpr uint32_t kHeap = 16u << 20;   // tracked [16 MB, 48 MB)
    constexpr uint32_t kHeapEnd = 48u << 20;
    constexpr uint32_t kAudio = 20u << 20;  // excluded [20 MB, 21 MB)
    constexpr uint32_t kAudioEnd = 21u << 20;
    constexpr uint32_t kWorker = 30u << 20;  // written by another thread: must become foreign
    constexpr uint32_t kWorkerEnd = 31u << 20;
    constexpr uint32_t kMid = kHeap + (kHeapEnd - kHeap) / 2;

    t_simulation = true;
    AddVectoredExceptionHandler(1, fault_handler);
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_READWRITE));
    if (!base)
    {
        std::printf("VirtualAlloc failed\n");
        return 1;
    }
    VirtualAlloc(base + kHeap, kHeapEnd - kHeap, MEM_COMMIT, PAGE_READWRITE);

    SnapshotRing ring(base, kSize, 8);
    g_ring = &ring;
    // Only the first half is game state at first; the second half joins mid-run (like a
    // guest allocation during a fight).
    ring.track(kHeap, kMid);
    ring.exclude(kAudio, kAudioEnd);

    std::mt19937 rng(42);
    auto scribble = [&](uint32_t limit) {
        const int writes = 1 + int(rng() % 400);
        for (int i = 0; i < writes; i++)
        {
            uint32_t addr = kHeap + rng() % (limit - kHeap - 8);
            if (addr >= kWorker - 8 && addr < kWorkerEnd)
                addr = kWorkerEnd + 64; // the worker's area is not the simulation's
            const uint64_t v = (uint64_t(rng()) << 32) | rng();
            std::memcpy(base + addr, &v, 8);
        }
    };
    uint32_t committed_end = kMid;
    auto copy_heap = [&]() {
        std::vector<uint8_t> c(kHeapEnd - kHeap);
        std::memcpy(c.data(), base + kHeap, committed_end - kHeap);
        return c;
    };
    auto equal_except_audio = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        for (size_t off = 0; off < a.size(); off += 4096)
        {
            if ((off >= kAudio - kHeap && off < kAudioEnd - kHeap) || (off >= kWorker - kHeap && off < kWorkerEnd - kHeap))
                continue;
            if (std::memcmp(a.data() + off, b.data() + off, 4096) != 0)
                return false;
        }
        return true;
    };

    scribble(committed_end);
    ring.begin(0);

    std::atomic<bool> stop{ false };
    std::thread worker([&] {
        std::mt19937 wr(7);
        while (!stop)
        {
            const uint32_t addr = kWorker + wr() % (kWorkerEnd - kWorker - 8);
            const uint64_t v = wr();
            std::memcpy(base + addr, &v, 8);
        }
    });

    std::map<int, std::vector<uint8_t>> reference;
    reference[0] = copy_heap();
    int failures = 0, loads = 0;
    int frame = 0;

    for (int step = 0; step < 1200; step++)
    {
        if (step == 400)
        {
            ring.track(kMid, kHeapEnd);
            committed_end = kHeapEnd;
        }
        scribble(committed_end);
        frame++;
        ring.save(frame);
        reference[frame] = copy_heap();
        reference.erase(frame - 9);

        if (rng() % 5 == 0)
        {
            // Write some garbage that the load must also discard, then roll back.
            scribble(committed_end);
            const int back = int(rng() % 9); // 0..8 frames (depth 8)
            const int target = frame - back;
            if (target < 0)
                continue;
            if (!ring.load(target))
            {
                std::printf("FAIL load(%d) refused at frame %d\n", target, frame);
                failures++;
                continue;
            }
            loads++;
            if (!equal_except_audio(copy_heap(), reference[target]))
            {
                std::printf("FAIL memory differs after load(%d) at frame %d\n", target, frame);
                if (++failures > 5)
                    break;
            }
            // Re-simulate back to the present with different "inputs", as the session does
            // after every rollback.
            for (int f = target + 1; f <= frame; f++)
            {
                scribble(committed_end);
                ring.save(f);
                reference[f] = copy_heap();
            }
        }
    }

    stop = true;
    worker.join();
    if (ring.foreign_pages() == 0)
    {
        std::printf("FAIL the worker thread's pages were not classified as foreign\n");
        failures++;
    }

    if (ring.load(frame - 20))
    {
        std::printf("FAIL load beyond depth accepted\n");
        failures++;
    }

    std::printf("snapshot test: %d frames, %d loads, last save %zu pages, shadow %zu KB\n", frame, loads, ring.pages_last_save(), ring.shadow_bytes() / 1024);
    std::printf("foreign pages: %zu\n", ring.foreign_pages());
    VirtualFree(base, 0, MEM_RELEASE);
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
