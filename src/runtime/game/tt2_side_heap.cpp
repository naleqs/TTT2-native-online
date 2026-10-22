// Side heap for the game's other threads (TU3).
//
// The game's main heap (holder 0x82E6AC00, a 184-byte heap object behind it; allocation core
// sub_82A68368(heap, size, align, ...), free core sub_82A679F0(heap, block)) is shared by the
// game logic, the main loop's draw pass and other threads (loaders, render and sound helpers).
// A rollback restores the heap's metadata, so a block allocated for the render thread after the
// restored frame becomes free again while it is still in use, and the game hands it out a second
// time: broken heap trees and crashes. Tekken6-rollback solves the same problem the same way:
// only game logic (here the main loop's update and the job threads) allocates from the main
// heap; every other allocation goes to a separate region that is not rolled back.
//
// - Only while rollback snapshots run (battles): loading threads allocate hundreds of MB before.
// - The region is 32 MB of guest virtual memory, excluded from the snapshots.
// - Blocks are size classes of 16 bytes up to 4 KB, powers of two above, with the size kept on
//   the host. Frees of side blocks come back here from any thread.
// - A re-simulated frame can free a side block a second time (the main thread freed it once
//   before the rollback). A freed chunk waits 1 s before it is handed out again, and a free of a
//   chunk that is not in use is ignored, so that neither breaks anything.
//
// TT2_SIDE_HEAP=0 turns it off (testing).

#include "../guest.h"
#include "../memory.h"
#include "../rollback/game_rollback.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kHeapHolder = 0x82E6AC00;
    constexpr uint32_t kRegionSize = 32u << 20;

    std::mutex g_mutex;
    uint32_t g_region = 0, g_top = 0;
    std::map<uint32_t, std::vector<uint32_t>> g_free;        // chunk size -> free chunks
    std::unordered_map<uint32_t, uint32_t> g_chunk_size;     // chunk -> chunk size
    std::unordered_set<uint32_t> g_unused;                    // free or waiting chunks
    std::deque<std::pair<uint32_t, std::chrono::steady_clock::time_point>> g_waiting;
    uint64_t g_allocs = 0, g_frees = 0, g_ignored = 0;

    bool enabled()
    {
        static const bool on = [] { const char* v = std::getenv("TT2_SIDE_HEAP"); return !v || *v != '0'; }();
        return on;
    }

    bool ensure_region()
    {
        if (g_region)
            return true;
        g_region = tt2::g_memory.virt.alloc(kRegionSize, 0x10000);
        g_top = g_region;
        if (g_region)
        {
            // The loader module's block pools (sub_824F75D8): size, count, free list.
            for (uint32_t holder : { 0x82EE5FF4u, 0x82EE5FF8u, 0x82EE5FFCu })
                if (const uint32_t pool = tt2::load32(holder))
                    tt2::rollback::preserve(pool, 16);
            tt2::log("side heap: %u MB at %08X for the game heap's blocks of other threads", kRegionSize >> 20, g_region);
        }
        return g_region != 0;
    }

    bool owns(uint32_t block)
    {
        return g_region && block >= g_region && block < g_region + kRegionSize;
    }

    uint32_t alloc(uint32_t size, uint32_t align)
    {
        std::lock_guard lock(g_mutex);
        if (!ensure_region())
            return 0;
        const auto now = std::chrono::steady_clock::now();
        while (!g_waiting.empty() && now - g_waiting.front().second > std::chrono::seconds(1))
        {
            const uint32_t c = g_waiting.front().first;
            g_free[g_chunk_size[c]].push_back(c);
            g_waiting.pop_front();
        }
        align = align < 16 ? 16 : align;
        uint32_t chunk = size <= 4096 ? (size + 15) & ~15u : 1u << (32 - __builtin_clz(size - 1));
        if (chunk == 0)
            chunk = 16;
        if (align <= 16)
            if (auto it = g_free.find(chunk); it != g_free.end() && !it->second.empty())
            {
                const uint32_t c = it->second.back();
                it->second.pop_back();
                g_unused.erase(c);
                g_allocs++;
                return c;
            }
        const uint32_t c = (g_top + align - 1) & ~(align - 1);
        if (c + chunk > g_region + kRegionSize)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                tt2::log("side heap: full, other threads' blocks go to the game heap again");
            }
            return 0;
        }
        g_top = c + chunk;
        g_chunk_size[c] = chunk;
        g_allocs++;
        return c;
    }

    // Game logic that calls into the sound library or the job/loader libraries allocates for
    // threads that keep using the block (the sound thread, the job pool, the loaders): those
    // blocks must not be rolled back either.
    bool called_from_shared_library(const PPCContext& ctx)
    {
        auto shared = [](uint32_t a) { return tt2::game::is_sound_code(a) || tt2::game::is_job_queue_code(a); };
        if (shared(uint32_t(ctx.lr)))
            return true;
        uint32_t r1 = ctx.r1.u32;
        for (int i = 0; i < 12; i++)
        {
            if (r1 < 0x40000000 || r1 >= 0x7F000000)
                break;
            const uint32_t caller = tt2::load32(r1);
            if (caller <= r1 || caller >= 0x7F000000)
                break;
            if (shared(tt2::load32(caller - 8)))
                return true;
            r1 = caller;
        }
        return false;
    }

    void free_block(uint32_t block)
    {
        std::lock_guard lock(g_mutex);
        if (!g_chunk_size.count(block) || g_unused.count(block))
        {
            g_ignored++;
            return;
        }
        g_unused.insert(block);
        g_waiting.push_back({ block, std::chrono::steady_clock::now() });
        g_frees++;
    }
}

namespace tt2::game
{
    std::pair<uint32_t, uint32_t> side_heap_range()
    {
        if (!enabled())
            return { 0, 0 };
        std::lock_guard lock(g_mutex);
        ensure_region();
        return { g_region, g_region + kRegionSize };
    }

    std::string side_heap_stats()
    {
        std::lock_guard lock(g_mutex);
        char buf[160];
        snprintf(buf, sizeof(buf), "side heap: %llu allocations, %llu frees, %llu repeated frees ignored, %u KB used",
                 (unsigned long long)g_allocs, (unsigned long long)g_frees, (unsigned long long)g_ignored, (g_top - g_region) / 1024);
        return buf;
    }
}

#if TT2_TU3
PPC_FUNC_IMPL(__imp__sub_82A68368);
PPC_FUNC(sub_82A68368)
{
    if (enabled() && tt2::rollback::snapshots_active() && ctx.r3.u32 == tt2::load32(kHeapHolder) &&
        (!tt2::rollback::simulating_now() || called_from_shared_library(ctx)))
    {
        if (const uint32_t block = alloc(ctx.r4.u32, ctx.r5.u32))
        {
            ctx.r3.u64 = block;
            return;
        }
    }
    __imp__sub_82A68368(ctx, base);
}

// The loaders' spatial tree objects (sub_82A40338(tree, ...), 0xA0 bytes) share pages with game
// state: their bytes are kept as they are when a rollback restores those pages.
PPC_FUNC_IMPL(__imp__sub_82A40338);
PPC_FUNC(sub_82A40338)
{
    if (enabled() && ctx.r3.u32 && !tt2::rollback::is_game_thread())
    {
        static std::mutex m;
        static std::unordered_set<uint32_t> seen;
        std::lock_guard lock(m);
        if (seen.insert(ctx.r3.u32).second)
            tt2::rollback::preserve(ctx.r3.u32, 0xA0);
    }
    __imp__sub_82A40338(ctx, base);
}

// Frees of game heap blocks outside game logic (other threads, the draw pass) would make
// whichever heap page they touch first in a frame "not game state" for good, while the heap's
// other pages still roll back: an inconsistent heap. They are queued and done by the main loop at
// a frame boundary at least kDeferFrames later, as game logic. The wait outlasts the deepest
// rollback: a block game logic handed to another thread within the rolled-back frames is still
// in use in the restored state (the re-run hands it over again, and its second free is dropped).
namespace
{
    constexpr uint64_t kDeferFrames = 8;
    struct DeferredFree
    {
        uint64_t frame;
        uint32_t heap, block;
    };
    std::mutex g_deferred_mutex;
    std::deque<DeferredFree> g_deferred;
    std::unordered_set<uint32_t> g_deferred_blocks;
    uint64_t g_boundaries = 0;
}

PPC_FUNC_IMPL(__imp__sub_82A679F0);
PPC_FUNC(sub_82A679F0)
{
    if (enabled() && owns(ctx.r4.u32))
    {
        free_block(ctx.r4.u32);
        return;
    }
    if (enabled() && tt2::rollback::snapshots_active() && !tt2::rollback::simulating_now() && ctx.r3.u32 == tt2::load32(kHeapHolder))
    {
        std::lock_guard lock(g_deferred_mutex);
        if (g_deferred_blocks.insert(ctx.r4.u32).second)
            g_deferred.push_back({ g_boundaries, ctx.r3.u32, ctx.r4.u32 });
        return;
    }
    __imp__sub_82A679F0(ctx, base);
}

namespace tt2::game
{
    void flush_deferred_frees()
    {
        std::vector<DeferredFree> frees;
        {
            std::lock_guard lock(g_deferred_mutex);
            g_boundaries++;
            while (!g_deferred.empty() && g_deferred.front().frame + kDeferFrames <= g_boundaries)
            {
                frees.push_back(g_deferred.front());
                g_deferred_blocks.erase(g_deferred.front().block);
                g_deferred.pop_front();
            }
        }
        for (const DeferredFree& f : frees)
            call_guest(0x82A679F0, f.heap, f.block);
    }
}
#endif

#if !TT2_TU3
namespace tt2::game
{
    void flush_deferred_frees() {} // the side heap is only set up for the Title Update 3 build
}
#endif
