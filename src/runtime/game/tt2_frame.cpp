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

void Tt2DrawBegin() { tt2::rollback::draw_pass(true); }
void Tt2DrawEnd() { tt2::rollback::draw_pass(false); }

namespace tt2::game
{
    // One iteration of the main loop, for re-simulated frames. The singleton initialisation
    // the original loop body guards (first frame only) has long happened by then.
    // Re-simulated frames skip the draw pass: the render thread may still be consuming the
    // previous frame's command lists, and nothing presented from a re-run frame is shown anyway
    // (Tekken6-rollback leaves the draw pass out the same way).
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
