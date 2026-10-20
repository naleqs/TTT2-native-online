#pragma once

// Connects the rollback session to the running game.
//
// The frame boundary is the top of the game's main loop (Tt2FrameBoundary hook, see
// game/tt2_frame.cpp); rollback/game_rollback.cpp runs the session there, restores snapshots
// and re-runs frames on mispredictions.

#include "input/input.h"

#include <cstdint>
#include <string>

namespace tt2::netglue
{
    struct Options
    {
        bool enabled = false;
        int local_player = 0;            // game port of the local player
        uint16_t local_port = 7000;
        std::string remote_host;         // 127.0.0.1 when the launcher relays a lobby match
        uint16_t remote_port = 7001;
        int input_delay = -1;            // -1 = automatic
        int max_prediction = 5;
        uint64_t build_id = 0;
    };

    void configure(const Options& options);
    bool active();

    // XamInputGetState: true when netplay supplies this port's state.
    bool input_for_port(int port, input::PadState& out);

    // The frame being simulated with intercepted inputs, or -1 (see rollback::current_frame).
    int frame(uint64_t* serial);

    // VdSwap: a frame was presented.
    void on_present();

    // One line for the window title / overlay ("ping 42 ms, delay 1, rollbacks 120").
    std::string status_line();

    void shutdown();
}
