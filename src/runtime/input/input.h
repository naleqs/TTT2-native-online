#pragma once

// Controllers only: up to two gamepads (any SDL3-supported pad: Xbox, PlayStation, Switch,
// generic XInput/DirectInput). The first pad connected is player 1, the second player 2.
// There is no keyboard mapping for gameplay by design.

#include <tt2/netplay/input.h>

#include <cstdint>

union SDL_Event;

namespace tt2::input
{
    constexpr int kPorts = 2;

    struct PadState
    {
        bool connected = false;
        uint16_t buttons = 0;           // XINPUT_GAMEPAD button bits
        uint8_t left_trigger = 0, right_trigger = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    void init();
    void shutdown();
    void handle_event(const SDL_Event& e); // main thread
    void poll();                           // main thread, once per host tick (after events)

    PadState read(int port);              // any thread
    void rumble(int port, uint16_t low, uint16_t high);
    int connected_count();

    // Netplay inputs are digital: the left stick becomes the d-pad, triggers become buttons,
    // so both machines see identical values.
    netplay::Input to_netplay(const PadState& s);
    PadState from_netplay(netplay::Input in);
}
