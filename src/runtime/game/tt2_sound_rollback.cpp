// Sound effects and rollback (TU3).
//
// The game starts its sound effects with sub_8259E948(id), which returns a handle (0: nothing
// started). A rollback re-runs frames that already started their sounds, so as in
// Tekken6-rollback sound ignores rollbacks: what played plays on, and a re-run frame starts
// nothing new. A play in a re-run frame that the first run of a nearby frame (3 frames either
// way: a corrected hit lands a frame or two earlier or later) already made gets that sound's
// handle back, so the game can still stop or change it; any other play in a re-run frame starts
// nothing and returns 0. A sound that only the corrected timeline has is not heard (rare).
//
// TT2_SOUND_ROLLBACK=0 turns this off (testing).

#include "../guest.h"
#include "../rollback/game_rollback.h"

#include <cstdlib>
#include <deque>
#include <mutex>

namespace
{
    struct Play
    {
        int frame;
        uint32_t id;
        uint32_t handle;
        bool claimed;
    };
    std::deque<Play> g_plays; // live plays of the last frames (the main thread only)
    uint64_t g_reused = 0, g_skipped = 0;

    bool enabled()
    {
        static const bool on = [] { const char* v = std::getenv("TT2_SOUND_ROLLBACK"); return !v || *v != '0'; }();
        return on;
    }
}

namespace tt2::game
{
    void sounds_on_rollback()
    {
        for (Play& p : g_plays)
            p.claimed = false;
    }

    std::string sound_rollback_stats()
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "sounds in re-run frames: %llu reused, %llu not started", (unsigned long long)g_reused,
                 (unsigned long long)g_skipped);
        return buf;
    }
}

#if TT2_TU3
PPC_FUNC_IMPL(__imp__sub_8259E948);
PPC_FUNC(sub_8259E948)
{
    const int frame = tt2::rollback::current_frame(nullptr);
    if (!enabled() || frame < 0 || !tt2::rollback::on_main_thread())
    {
        __imp__sub_8259E948(ctx, base);
        return;
    }
    const uint32_t id = ctx.r3.u32;
    if (tt2::rollback::resimulating())
    {
        Play* best = nullptr;
        int best_distance = 4;
        for (Play& p : g_plays)
            if (!p.claimed && p.id == id && std::abs(p.frame - frame) < best_distance)
            {
                best = &p;
                best_distance = std::abs(p.frame - frame);
            }
        if (best)
        {
            best->claimed = true;
            g_reused++;
            ctx.r3.u64 = best->handle;
        }
        else
        {
            g_skipped++;
            ctx.r3.u64 = 0;
        }
        return;
    }
    __imp__sub_8259E948(ctx, base);
    g_plays.push_back({ frame, id, ctx.r3.u32, false });
    while (!g_plays.empty() && g_plays.front().frame < frame - 16)
        g_plays.pop_front();
}
#endif
