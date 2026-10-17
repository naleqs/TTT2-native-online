#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>

namespace tt2::input
{
    namespace
    {
        std::mutex g_mutex;
        SDL_Gamepad* g_pads[kPorts]{};
        SDL_JoystickID g_ids[kPorts]{};
        PadState g_state[kPorts];

        constexpr int16_t kStickDeadzone = 7849;   // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
        constexpr uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

        void refresh(int port)
        {
            SDL_Gamepad* pad = g_pads[port];
            PadState s;
            if (pad)
            {
                s.connected = true;
                auto btn = [&](SDL_GamepadButton b, uint16_t bit) {
                    if (SDL_GetGamepadButton(pad, b))
                        s.buttons |= bit;
                };
                using namespace netplay::button;
                btn(SDL_GAMEPAD_BUTTON_DPAD_UP, dpad_up);
                btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, dpad_down);
                btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, dpad_left);
                btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, dpad_right);
                btn(SDL_GAMEPAD_BUTTON_START, start);
                btn(SDL_GAMEPAD_BUTTON_BACK, back);
                btn(SDL_GAMEPAD_BUTTON_LEFT_STICK, left_thumb);
                btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK, right_thumb);
                btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, left_shoulder);
                btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, right_shoulder);
                // Positional layout: the bottom face button is A on every pad (Cross on
                // PlayStation, B on Nintendo), so muscle memory matches the Xbox layout.
                btn(SDL_GAMEPAD_BUTTON_SOUTH, a);
                btn(SDL_GAMEPAD_BUTTON_EAST, b);
                btn(SDL_GAMEPAD_BUTTON_WEST, x);
                btn(SDL_GAMEPAD_BUTTON_NORTH, y);

                auto trigger = [&](SDL_GamepadAxis axis) { return uint8_t(std::clamp(SDL_GetGamepadAxis(pad, axis), int16_t(0), int16_t(32767)) * 255 / 32767); };
                s.left_trigger = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
                s.right_trigger = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
                // SDL's Y axes point down; XInput's point up.
                auto axis = [&](SDL_GamepadAxis a, bool invert) {
                    const int v = SDL_GetGamepadAxis(pad, a);
                    return int16_t(std::clamp(invert ? -v - 1 : v, -32768, 32767));
                };
                s.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
                s.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);
                s.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
                s.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
            }
            g_state[port] = s;
        }

        void open(SDL_JoystickID id)
        {
            for (int p = 0; p < kPorts; p++)
                if (g_pads[p] && g_ids[p] == id)
                    return;
            for (int p = 0; p < kPorts; p++)
            {
                if (!g_pads[p])
                {
                    g_pads[p] = SDL_OpenGamepad(id);
                    g_ids[p] = id;
                    if (g_pads[p])
                        SDL_SetGamepadPlayerIndex(g_pads[p], p);
                    return;
                }
            }
            // More than two pads: ignored until a port frees up.
        }
    }

    void init()
    {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
        {
            std::lock_guard lock(g_mutex);
            for (int i = 0; i < count; i++)
                open(ids[i]);
            SDL_free(ids);
        }
    }

    void shutdown()
    {
        std::lock_guard lock(g_mutex);
        for (auto& p : g_pads)
            if (p)
            {
                SDL_CloseGamepad(p);
                p = nullptr;
            }
    }

    void handle_event(const SDL_Event& e)
    {
        std::lock_guard lock(g_mutex);
        switch (e.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
            open(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (int p = 0; p < kPorts; p++)
                if (g_pads[p] && g_ids[p] == e.gdevice.which)
                {
                    SDL_CloseGamepad(g_pads[p]);
                    g_pads[p] = nullptr;
                }
            break;
        default:
            break;
        }
        for (int p = 0; p < kPorts; p++)
            refresh(p);
    }

    PadState read(int port)
    {
        if (port < 0 || port >= kPorts)
            return {};
        std::lock_guard lock(g_mutex);
        return g_state[port];
    }

    void poll()
    {
        std::lock_guard lock(g_mutex);
        for (int p = 0; p < kPorts; p++)
            refresh(p);
    }

    void rumble(int port, uint16_t low, uint16_t high)
    {
        std::lock_guard lock(g_mutex);
        if (port >= 0 && port < kPorts && g_pads[port])
            SDL_RumbleGamepad(g_pads[port], low, high, 200);
    }

    int connected_count()
    {
        std::lock_guard lock(g_mutex);
        return int(std::count_if(std::begin(g_pads), std::end(g_pads), [](auto* p) { return p != nullptr; }));
    }

    netplay::Input to_netplay(const PadState& s)
    {
        using namespace netplay::button;
        netplay::Input in = s.buttons & 0xF3FF;
        if (s.lx < -kStickDeadzone) in |= dpad_left;
        if (s.lx > kStickDeadzone) in |= dpad_right;
        if (s.ly > kStickDeadzone) in |= dpad_up;
        if (s.ly < -kStickDeadzone) in |= dpad_down;
        if (s.left_trigger > kTriggerThreshold) in |= left_trigger;
        if (s.right_trigger > kTriggerThreshold) in |= right_trigger;
        return in & all_valid;
    }

    PadState from_netplay(netplay::Input in)
    {
        using namespace netplay::button;
        PadState s;
        s.connected = true;
        s.buttons = uint16_t(in & 0xF3FF);
        s.left_trigger = (in & left_trigger) ? 255 : 0;
        s.right_trigger = (in & right_trigger) ? 255 : 0;
        return s;
    }
}
