#pragma once

// Connects the rollback session to the running game.
//
// Frame boundary: the first read of controller port 0 after each presented frame, the same
// point Tekken6-rollback uses on the PS3 version (its cellPadGetData hook). Until the game's
// main-loop function is located (config/recomp/tt2_hooks.toml), frames cannot be re-run, so
// the session runs with a prediction window of 0: classic delay-based lockstep with the
// automatic input delay. With the hook in place the same session switches on rollback,
// using rollback/snapshot.h for the state.

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

    // VdSwap: a frame was presented.
    void on_present();

    // One line for the window title / overlay ("ping 42 ms, delay 1, rollbacks 120").
    std::string status_line();

    void shutdown();
}
