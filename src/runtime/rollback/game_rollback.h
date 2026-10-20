#pragma once

// Rollback for the running game, built on the frame-boundary hook (game/tt2_frame.cpp).
//
// Modes:
//   * netplay  - the rollback session (src/netplay) decides each frame's inputs; on a
//                misprediction the state at the first wrong frame is restored from the
//                snapshot ring and the frames up to the present are re-run without presenting.
//   * stress   - offline determinism test (TT2_ROLLBACK_STRESS=interval[,depth]): every
//                `interval` frames, roll back `depth` frames and re-run them with the
//                recorded inputs; each re-run frame must change memory exactly like the
//                original did. This is how Tekken6-rollback was validated as well.
//   * off      - the hook only records the loop registers.

#include "../input/input.h"
#include "../netplay_glue.h"

#include <cstdint>
#include <string>

namespace tt2::rollback
{
    // The main loop's invariant registers, as game/tt2_frame.cpp's frame hook passes them.
    struct FrameRegs
    {
        uint32_t regs[6] = {};
    };

    // The Tt2FrameBoundary hook: top of every main loop iteration, on the game's main thread.
    void frame_boundary(const FrameRegs& regs);

    // The main thread enters/leaves the frame's draw pass (pages first written while drawing
    // are presentation state and never rolled back).
    void draw_pass(bool drawing);

    // True while a frame is being re-simulated (no presenting, no new sounds).
    bool resimulating();

    // Inputs are frozen at the frame boundary in netplay and stress mode, so a frame and its
    // re-simulation read identical values. False when not intercepting.
    bool input_for_port(int port, input::PadState& out);

    // The OS is about to write guest memory for the current thread (ReadFile): snapshot
    // tracking must see it, protected pages would make the call fail instead of faulting.
    void note_host_write(uint32_t address, uint32_t size);

    void configure_netplay(const netglue::Options& options);
    void configure_stress(int interval, int depth, int start);

    // The frame being simulated while inputs are intercepted (-1 otherwise), and a number that
    // changes every time a frame starts, live or re-run: per-frame state derived from the
    // inputs (keystrokes) is rebuilt for each run of a frame.
    int current_frame(uint64_t* serial);

    bool netplay_active();
    std::string status_line();
    void shutdown();
}

namespace tt2::game
{
    void run_frame_body(const rollback::FrameRegs& regs);
}
