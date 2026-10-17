#pragma once

#include <cstdint>

namespace tt2::netplay
{
    // One player's input for one frame, as it travels over the network and enters the
    // rollback session. Analog sticks are folded into the d-pad bits and the triggers into
    // two button bits *before* this point, so both machines simulate with bit-identical
    // inputs no matter which controller each player uses.
    //
    // The low 16 bits use the XINPUT_GAMEPAD button layout the game reads through
    // XamInputGetState; bits 16/17 carry the triggers.
    using Input = uint32_t;

    namespace button
    {
        constexpr Input dpad_up = 0x0001;
        constexpr Input dpad_down = 0x0002;
        constexpr Input dpad_left = 0x0004;
        constexpr Input dpad_right = 0x0008;
        constexpr Input start = 0x0010;
        constexpr Input back = 0x0020;
        constexpr Input left_thumb = 0x0040;
        constexpr Input right_thumb = 0x0080;
        constexpr Input left_shoulder = 0x0100;
        constexpr Input right_shoulder = 0x0200;
        constexpr Input a = 0x1000;
        constexpr Input b = 0x2000;
        constexpr Input x = 0x4000;
        constexpr Input y = 0x8000;
        constexpr Input left_trigger = 0x10000;
        constexpr Input right_trigger = 0x20000;

        constexpr Input all_valid = 0x3F3FF;
    }

    constexpr int kMaxPlayers = 2;
}
