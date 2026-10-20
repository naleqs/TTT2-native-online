#include "input.h"
#include "../netplay_glue.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>
#include <string>
#include <chrono>
#include <cstdlib>
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

        // TT2_DEBUG_KEYBOARD=1: the keyboard acts as player 1's pad, for driving the game from
        // test scripts. Off by default; the game is played with controllers.
        bool g_debug_keyboard = false;

        // TT2_INPUT_SCRIPT=ms=button[,ms=button...]: press player 1's buttons (150 ms each) at
        // fixed times after start, for unattended test runs (ms=button:duration to hold longer).
        // Buttons: a b x y start back up down left right lb rb.
        struct ScriptedPress
        {
            int64_t at_ms;
            uint16_t bit;
            int64_t duration_ms;
        };
        std::vector<ScriptedPress> g_script;
        std::chrono::steady_clock::time_point g_start;

        void load_script(const char* text)
        {
            using namespace netplay::button;
            const std::pair<const char*, uint16_t> names[] = {
                { "a", a }, { "b", b }, { "x", x }, { "y", y }, { "start", start }, { "back", back }, { "up", dpad_up },
                { "down", dpad_down }, { "left", dpad_left }, { "right", dpad_right }, { "lb", left_shoulder }, { "rb", right_shoulder },
            };
            std::string spec(text);
            size_t pos = 0;
            while (pos < spec.size())
            {
                const size_t end = std::min(spec.find(',', pos), spec.size());
                const std::string item = spec.substr(pos, end - pos);
                const size_t eq = item.find('=');
                if (eq != std::string::npos)
                {
                    // ms=button or ms=button:duration_ms (default 150)
                    std::string button = item.substr(eq + 1);
                    int64_t duration = 150;
                    if (const size_t colon = button.find(':'); colon != std::string::npos)
                    {
                        duration = std::atoll(button.c_str() + colon + 1);
                        button.resize(colon);
                    }
                    for (auto& [name, bit] : names)
                        if (button == name)
                            g_script.push_back({ std::atoll(item.c_str()), bit, duration });
                }
                pos = end + 1;
            }
            g_start = std::chrono::steady_clock::now();
        }

        void scripted(PadState& s)
        {
            // In netplay the script follows the game's frames (60 per second), so both players'
            // presses land on the same frames however fast each PC runs.
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
            if (const int frame = netglue::frame(nullptr); frame >= 0)
                now = int64_t(frame) * 1000 / 60;
            for (const ScriptedPress& p : g_script)
                if (now >= p.at_ms && now < p.at_ms + p.duration_ms)
                    s.buttons |= p.bit;
            s.connected = true;
        }

        void debug_keyboard(PadState& s)
        {
            const bool* k = SDL_GetKeyboardState(nullptr);
            using namespace netplay::button;
            const struct { SDL_Scancode key; uint16_t bit; } map[] = {
                { SDL_SCANCODE_UP, dpad_up }, { SDL_SCANCODE_DOWN, dpad_down }, { SDL_SCANCODE_LEFT, dpad_left },
                { SDL_SCANCODE_RIGHT, dpad_right }, { SDL_SCANCODE_RETURN, start }, { SDL_SCANCODE_BACKSPACE, back },
                { SDL_SCANCODE_Z, a }, { SDL_SCANCODE_X, b }, { SDL_SCANCODE_A, x }, { SDL_SCANCODE_S, y },
                { SDL_SCANCODE_Q, left_shoulder }, { SDL_SCANCODE_W, right_shoulder },
            };
            for (const auto& m : map)
                if (k[m.key])
                    s.buttons |= m.bit;
            s.connected = true;
        }

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
            if (port == 0 && g_debug_keyboard)
                debug_keyboard(s);
            if (port == 0 && !g_script.empty())
                scripted(s);
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
        if (const char* v = std::getenv("TT2_DEBUG_KEYBOARD"); v && *v == '1')
            g_debug_keyboard = true;
        if (const char* v = std::getenv("TT2_INPUT_SCRIPT"); v && *v)
            load_script(v);
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
