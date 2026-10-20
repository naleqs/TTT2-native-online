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
#include "../rollback/game_rollback.h"

#if TT2_TU3
void Tt2FrameBoundary(PPCRegister& r20, PPCRegister& r21, PPCRegister& r26, PPCRegister& r27, PPCRegister& r30)
{
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
PPC_FUNC_IMPL(__imp__sub_822D9A30);
PPC_FUNC(sub_822D9A30)
{
    const uint32_t round = tt2::call_guest(0x825B7370);
    const int32_t before = round ? int32_t(tt2::load32(round + 0x2EC)) : 0;
    __imp__sub_822D9A30(ctx, base);
    if (round && int32_t(tt2::load32(round + 0x2EC)) < before)
        tt2::rollback::note_round_tick();
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
    uint32_t render_thread_entry()
    {
#if TT2_TU3
        return 0x82AE7090;
#else
        return 0; // not identified for the retail build yet
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
        return { { 0x830B0000, 0x830C0000 } }; // the sound engine object (0x830B8E70) and its data
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
