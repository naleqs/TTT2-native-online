// Game-specific glue for Tekken Tag Tournament 2 (USA, Europe). Setup only accepts the retail
// Default.xex (SHA-1 a942090b7c293d0ce63346ed8a2974448135d0dd), optionally patched with the
// player's own Title Update 3 (TT2_TU3: patched SHA-1 04b49c51f5358f9ce6cdb7d04d50aa05722a5465),
// so the addresses below are fixed per version.
//
// Retail: the main loop is sub_8256CB80. One iteration (loc_8256CBE8) is one game frame:
//
//   sub_82AA88D8()                 empty in the retail build (profiling marker)
//   sub_825499E0(*r23)             system/game update; reads the controllers
//   sub_8267BB78(r29)              manager update
//   sub_82650068(r30)              manager update
//   sub_82650388(r30)              manager update
//   sub_825D6E98(r24)              render
//   sub_825D3AF0(r24)              render + present (VdSwap)
//   sub_825D3BC8(r24)              returns non-zero when the game quits
//
// Title Update 3: the main loop is sub_82583288, iteration loc_825832FC. Same structure with the
// registers renamed (r21, r26, r30, r20 for r23, r29, r30, r24), plus a block the update added:
//
//   sub_8255EFF0(*r21)             system/game update
//   sub_8269AEF0(r26)              manager update
//   sub_8266D550(r30), sub_8266D8C0(r30)
//   if (sub_826B8060(r27)) { x = sub_826B8060(r27); o = sub_82666E90(x);
//       if (o && sub_8266ACE0(o, 0)) sub_82564DB8(sub_8266AD20(o, 0) != 0); }
//   sub_825EF0B8(r20), sub_825EBCC8(r20)   draw pass
//   sub_825EBDA0(r20)              returns non-zero when the game quits
//
// The loop's invariant registers are set up before the loop. The Tt2FrameBoundary mid-asm hook
// (config/recomp/tt2_hooks*.toml) runs at the top of every iteration: that is the rollback frame
// boundary, the counterpart of Tekken6-rollback's task-walk hook.

#include "../guest.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "../rollback/game_rollback.h"
#include "../kernel/threads.h"
#include <chrono>
#include <mutex>

namespace tt2::game
{
    std::atomic<uint64_t> g_loop_iterations{ 0 }; // main loop iterations (latency diagnostics)
}

bool scene_step_is_game();
namespace tt2::game
{
    void run_scene_update(int frame);
}

#if TT2_TU3
void Tt2FrameBoundary(PPCRegister& r20, PPCRegister& r21, PPCRegister& r26, PPCRegister& r27, PPCRegister& r30)
{
    tt2::game::g_loop_iterations++;
    tt2::rollback::frame_boundary({ { r21.u32, r20.u32, r26.u32, r30.u32, r27.u32 } });
}
#else
void Tt2FrameBoundary(PPCRegister& r23, PPCRegister& r24, PPCRegister& r29, PPCRegister& r30)
{
    tt2::rollback::frame_boundary({ { r23.u32, r24.u32, r29.u32, r30.u32 } });
}
#endif

// The battle scene's update (TU3 sub_825DF228; called from the draw pass while the VS screen,
// the intros, the fight and the results run): netplay rolls back only while it runs.
#if TT2_TU3
PPC_FUNC_IMPL(__imp__sub_825DF228);
PPC_FUNC(sub_825DF228)
{
    tt2::rollback::note_battle_scene();
    __imp__sub_825DF228(ctx, base);
}
#endif

// The fight update (TU3 sub_822D9A30, called by the battle scene while a round is fought): it
// takes the round object from sub_825B7370 and counts its clock (+0x2EC, frames) down unless the
// round is on hold (+0x300) or the game is paused. A tick means the players have control.
#if TT2_TU3
namespace
{
    // TT2_FIGHT_CHECK=1 (desync hunting): the fighters' health and positions at each frame
    // boundary (after the job threads finished). A re-run of a frame that ends with different
    // values is logged, as is the first frame of each second, so two players' logs can be compared.
    bool fight_check_enabled()
    {
        static const bool enabled = [] { const char* v = std::getenv("TT2_FIGHT_CHECK"); return v && *v == '1'; }();
        return enabled;
    }
    std::atomic<uint32_t> g_fighters[2]{};
}

namespace tt2::game
{
    void check_fight(int frame)
    {
        const uint32_t a = g_fighters[0].load(), b = g_fighters[1].load();
        if (!fight_check_enabled() || !a || !b || frame < 0)
            return;
        char line[256];
        int n = 0;
        for (uint32_t p : { a, b, load32(a + 172), load32(b + 172) })
            n += snprintf(line + n, sizeof(line) - n, " %d", p ? int32_t(load32(p + 96)) : -1);
        for (uint32_t p : { a, b })
        {
            const uint32_t t = call_guest(0x82396828, p);
            // TT2_WATCH_FIGHTER=frames: that many checks in, watch fighter 1's position.
            static const int watch_after = [] { const char* v = std::getenv("TT2_WATCH_FIGHTER"); return v ? std::atoi(v) : 0; }();
            static int checks = 0;
            if (watch_after && p == a && ++checks == watch_after)
                watch_now(t + 52);
            for (int i = 0; i < 3; i++)
                n += snprintf(line + n, sizeof(line) - n, " %08X", load32(t + 48 + 4 * i));
        }
        static std::map<int, std::string> seen;
        auto it = seen.find(frame);
        static int same = 0;
        if (it != seen.end() && it->second == line && same++ < 3)
            log("fight check: frame %d re-ran the same", frame);
        if (it != seen.end() && it->second != line)
            log("fight check: frame %d re-ran differently:%s (was%s)", frame, line, it->second.c_str());
        else if (it == seen.end() && frame % 60 == 0)
            log("fight check: frame %d:%s", frame, line);
        // TT2_FIGHT_LOG_ALL=1: every frame (live, and re-runs marked), to find the first frame
        // where a game leaves a reference run.
        static const bool all = [] { const char* v = std::getenv("TT2_FIGHT_LOG_ALL"); return v && *v == '1'; }();
        if (all)
            log("fight all: %d %s%s", frame, rollback::rolling_back() ? "R" : "L", line);
        seen[frame] = line;
        while (seen.size() > 64)
            seen.erase(seen.begin());
    }
}

PPC_FUNC_IMPL(__imp__sub_822D9A30);
PPC_FUNC(sub_822D9A30)
{
    const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
    const uint32_t round = tt2::call_guest(0x825B7370);
    const int32_t before = round ? int32_t(tt2::load32(round + 0x2EC)) : 0;
    __imp__sub_822D9A30(ctx, base);
    if (round && int32_t(tt2::load32(round + 0x2EC)) < before)
        tt2::rollback::note_round_tick();
    g_fighters[0] = a;
    g_fighters[1] = b;
}
#endif

// The scenes' frame step (TU3 sub_82566650(task manager, task mask)): the task walk
// sub_8226E768(game, mask), which runs all game logic including the fight, then
// sub_82566468, which measures the frame time, renders and presents. Both are called from the
// main loop's draw pass. The task walk's writes are game state (rolled back), and a
// re-simulated frame runs only the task walk.
#if TT2_TU3
PPC_FUNC_IMPL(__imp__sub_8226E768);
PPC_FUNC(sub_8226E768)
{
    const bool previous = tt2::rollback::set_simulation(true);
    __imp__sub_8226E768(ctx, base);
    tt2::rollback::set_simulation(previous);
}

// The scene's update (TU3 sub_825E04F0(scene, x), called from the scene manager's step
// sub_825EF0B8, the first half of the draw pass): in a battle it runs the round flow and the
// fighters (sub_8262E418 -> ... -> the fight update sub_822D9A30) and the tasks' input step
// (sub_82568188), where the fighters read the controllers. It is game logic: rolled back, and
// re-run frames repeat it with the live frame's arguments, before the task walk.
// TT2_SCENE_STEP_GAME=0 turns this off (testing).
bool scene_step_is_game()
{
    static const bool on = [] { const char* v = std::getenv("TT2_SCENE_STEP_GAME"); return !v || *v != '0'; }();
    return on;
}

namespace
{
    struct SceneUpdateArgs
    {
        int frame = -1;
        uint32_t scene = 0, arg = 0;
    };
    SceneUpdateArgs g_scene_update[64];
}

PPC_FUNC_IMPL(__imp__sub_825E04F0);
PPC_FUNC(sub_825E04F0)
{
    if (!scene_step_is_game())
    {
        __imp__sub_825E04F0(ctx, base);
        return;
    }
    if (!tt2::rollback::resimulating())
        if (const int frame = tt2::rollback::current_frame(nullptr); frame >= 0)
            g_scene_update[frame % 64] = { frame, ctx.r3.u32, ctx.r4.u32 };
    const bool previous = tt2::rollback::set_simulation(true);
    __imp__sub_825E04F0(ctx, base);
    tt2::rollback::set_simulation(previous);
}

namespace tt2::game
{
    void run_scene_update(int frame)
    {
        const SceneUpdateArgs& a = g_scene_update[frame % 64];
        if (scene_step_is_game() && a.frame == frame)
            call_guest(0x825E04F0, a.scene, a.arg);
    }
}

PPC_FUNC_IMPL(__imp__sub_82566650);
PPC_FUNC(sub_82566650)
{
    if (!tt2::rollback::resimulating())
    {
        __imp__sub_82566650(ctx, base);
        return;
    }
    tt2::call_guest(0x8226E768, tt2::load32(0x82E6B6F8), ctx.r4.u32); // the game object, the task mask
}
#endif

void Tt2DrawBegin() { tt2::rollback::draw_pass(true); }
void Tt2DrawEnd() { tt2::rollback::draw_pass(false); }

namespace tt2::game
{
    // One iteration of the main loop, for re-simulated frames. The singleton initialisation
    // the original loop body guards (first frame only) has long happened by then.
    // Re-simulated frames skip the draw pass: the render thread may still be consuming the
    // previous frame's command lists, and nothing presented from a re-run frame is shown anyway
    // (Tekken6-rollback leaves the draw pass out the same way).
    bool is_sound_code(uint32_t a)
    {
#if TT2_TU3
        return (a >= 0x82B1C000 && a < 0x82B30000) || (a >= 0x82CF0000 && a < 0x82D10000);
#else
        (void)a;
        return false;
#endif
    }

    bool is_job_queue_code(uint32_t a)
    {
#if TT2_TU3
        // sub_82AC43C8 -> sub_82AF83E0 -> sub_82AD2DF8 -> sub_82AE2438 (queue at 0x82F6D000),
        // run by the pool threads at 0x82AE7090.
        // Also the library the loader threads (sub_824F5458) work through (0x82BA0000-0x82BC0000):
        // the main thread queues requests in it, a rollback must not rewind them under the loader.
        // And the streaming library the loaders read sound data through (0x82A40000-0x82A60000).
        // And the loader module itself (0x824F5000-0x824F8000): its block pools (sub_824F75D8,
        // the streaming library's allocator) are shared by the main thread and the loaders.
        return (a >= 0x82AC0000 && a < 0x82B10000) || (a >= 0x82BA0000 && a < 0x82BC0000) || (a >= 0x82A40000 && a < 0x82A60000) ||
               (a >= 0x824F5000 && a < 0x824F8000);
#else
        (void)a;
        return false;
#endif
    }

    uint32_t render_thread_entry()
    {
#if TT2_TU3
        return 0x82AE7090;
#else
        return 0; // not identified for the retail build yet
#endif
    }

    std::vector<uint32_t> game_worker_entries()
    {
#if TT2_TU3
        return { 0x82258D58 }; // the job threads: character movement (sub_824BDCA0 writes positions)
#else
        return {};
#endif
    }

    uint32_t thread_routine(uint32_t start_address, uint32_t start_context)
    {
#if TT2_TU3
        // sub_82A74760(context): calls context[0](context[1])
        if (start_address == 0x82A74760 && start_context)
            return load32(start_context);
#endif
        (void)start_context;
        return start_address;
    }

    bool thread_waiting_in_guest(uint32_t r1)
    {
#if TT2_TU3
        // sub_82AE4438(object, value): the job pool's wait until object+16 == value. Waiters
        // that wake with the condition still false set the event again for the others, so two
        // waiters keep waking each other (the pool's dispatcher, sub_82AD2AF8).
        for (int i = 0; i < 3; i++)
        {
            if (r1 < 0x40000000 || r1 >= 0x7F000000)
                break;
            const uint32_t caller = load32(r1);
            if (caller <= r1 || caller >= 0x7F000000)
                break;
            const uint32_t lr = load32(caller - 8);
            if (lr >= 0x82AE4438 && lr < 0x82AE44B8)
                return true;
            r1 = caller;
        }
#else
        (void)r1;
#endif
        return false;
    }

    bool worker_idle(uint32_t r1)
    {
#if TT2_TU3
        // sub_82258BD8, the job threads' loop: it waits for work in the call returning to 0x82258D3C.
        for (int i = 0; i < 3; i++)
        {
            if (r1 < 0x40000000 || r1 >= 0x7F000000)
                break;
            const uint32_t caller = load32(r1);
            if (caller <= r1 || caller >= 0x7F000000)
                break;
            if (load32(caller - 8) == 0x82258D3C)
                return true;
            r1 = caller;
        }
        return false;
#else
        (void)r1;
        return true;
#endif
    }

    uint32_t sound_thread_entry()
    {
#if TT2_TU3
        return 0x82D032D0;
#else
        return 0;
#endif
    }

    void align_frame_counter(int netplay_frame)
    {
#if TT2_TU3
        store32(0x82E6AD14, uint32_t(netplay_frame));
#else
        (void)netplay_frame;
#endif
    }

    std::vector<std::pair<uint32_t, uint32_t>> excluded_from_rollback()
    {
#if TT2_TU3
        std::vector<std::pair<uint32_t, uint32_t>> out{
            { 0x830B0000, 0x830C0000 }, // the sound engine object (0x830B8E70) and its data
            { 0xA0000000, 0xC0000000 }, // physical memory: GPU resources and audio buffers (the sound thread's stream decoders live there)
        };
        if (const auto side = side_heap_range(); side.first)
            out.push_back(side); // other threads' heap blocks (tt2_side_heap.cpp)
        return out;
#else
        return {};
#endif
    }

    void run_frame_body(const rollback::FrameRegs& f)
    {
        // regs: 0 = system object pointer, 1 = renderer, 2 and 3 = managers, 4 = TU3's addition
        const uint32_t* r = f.regs;
#if TT2_TU3
        call_guest(0x8255EFF0, load32(r[0]));
        call_guest(0x8269AEF0, r[2]);
        call_guest(0x8266D550, r[3]);
        call_guest(0x8266D8C0, r[3]);
        if (call_guest(0x826B8060, r[4]))
        {
            const uint32_t o = call_guest(0x82666E90, call_guest(0x826B8060, r[4]));
            if (o && call_guest(0x8266ACE0, o, 0))
                call_guest(0x82564DB8, call_guest(0x8266AD20, o, 0) != 0 ? 1u : 0u);
        }
        // The scene update (sub_825EBCC8: the scene's virtual update, which runs the task walk and
        // then renders and presents). Re-runs go through it for the task walk; the render half is
        // skipped by the sub_82566650 hook below.
        static const bool full_step = [] { const char* v = std::getenv("TT2_RERUN_FULL_SCENE_STEP"); return v && *v == '1'; }();
        if (full_step)
            call_guest(0x825EF0B8, r[1]);
        else
            run_scene_update(rollback::current_frame(nullptr));
        call_guest(0x825EBCC8, r[1]);
        call_guest(0x825EBDA0, r[1]);
#else
        call_guest(0x825499E0, load32(r[0]));
        call_guest(0x8267BB78, r[2]);
        call_guest(0x82650068, r[3]);
        call_guest(0x82650388, r[3]);
        call_guest(0x825D3BC8, r[1]);
#endif
    }
}


#if !TT2_TU3
namespace tt2::game
{
    void check_fight(int) {} // the fighters' addresses are only known for the Title Update 3 build
}
#endif
