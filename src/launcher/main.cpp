// TTT2 Native Online launcher: first-time setup, local versus, online lobby, direct connect.
// Fully usable with a controller (D-pad/stick to move, A to select, B to go back); text is
// entered with an on-screen keyboard.

#include "avatar.h"
#include "config.h"
#include "osk.h"
#include "process.h"

#include <tt2/netplay/transport.h>
#ifdef TT2_WITH_LOBBY
#include <tt2/netplay/lobby.h>
#endif

#include <SDL3/SDL.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace tt2::launcher;

namespace
{
    enum class Screen
    {
        Home,
        Setup,
        Lobby,
        Direct,
        Settings,
        InGame,
    };

    struct App
    {
        fs::path root;      // folder with Setup.bat
        fs::path native;    // root/native
        Config config;
        Screen screen = Screen::Home;
        std::optional<BuildInfo> build;
        std::unique_ptr<ChildProcess> child;   // setup or game
        std::string message;
        Osk osk;
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* avatar = nullptr;          // our profile picture (null: none)
        // Other players' pictures, by peer id (with the PNG they were made from).
        std::map<std::string, std::pair<std::string, SDL_Texture*>> peer_avatars;
#ifdef TT2_WITH_LOBBY
        std::unique_ptr<tt2::lobby::Lobby> lobby;
        std::unique_ptr<tt2::lobby::LoopbackRelay> relay;
        std::string challenger;                 // peer that challenged us
        std::string lobby_log;
#endif
    };

    fs::path avatar_file(const App& app) { return app.native / "profile" / "avatar.png"; }

    SDL_Texture* make_texture(SDL_Renderer* renderer, const Rgba& image)
    {
        if (image.pixels.empty())
            return nullptr;
        SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
        if (t)
        {
            SDL_UpdateTexture(t, nullptr, image.pixels.data(), image.width * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        }
        return t;
    }

    void reload_avatar(App& app)
    {
        if (app.avatar)
            SDL_DestroyTexture(app.avatar);
        app.avatar = nullptr;
        Rgba image;
        if (fs::exists(avatar_file(app)) && decode_image_file(avatar_file(app), image))
            app.avatar = make_texture(app.renderer, image);
    }

    // A square picture, or an empty frame of the same size when there is none.
    void avatar_image(SDL_Texture* t, float size)
    {
        if (t)
            ImGui::Image(ImTextureID(t), ImVec2(size, size));
        else
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(90, 90, 110, 255), 6.0f);
            ImGui::Dummy(ImVec2(size, size));
        }
    }

    void refresh_build(App& app)
    {
        app.build = read_build_info(app.native / "bin" / "build-info.json");
        if (app.build && !fs::exists(app.native / "bin" / "tt2.exe"))
            app.build.reset();
    }

    bool big_button(const char* label, bool enabled = true)
    {
        if (!enabled)
            ImGui::BeginDisabled();
        const bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, 56));
        if (!enabled)
            ImGui::EndDisabled();
        return pressed && enabled;
    }

    std::wstring widen_utf8(const std::string& s)
    {
        if (s.empty())
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    // The two players' names as the game shows them (its gamertags), the same on both PCs.
    std::vector<std::wstring> name_args(const std::string& p1, const std::string& p2)
    {
        return { L"--name1", widen_utf8(p1), L"--name2", widen_utf8(p2) };
    }

    void start_game(App& app, const std::vector<std::wstring>& extra)
    {
        std::vector<std::wstring> args;
        if (app.config.fullscreen)
            args.push_back(L"--fullscreen");
        args.insert(args.end(), extra.begin(), extra.end());
        app.child = ChildProcess::start(app.native / "bin" / "tt2.exe", args, app.native / "bin", false);
        if (!app.child)
            app.message = "Could not start the game.";
        else
            app.screen = Screen::InGame;
    }

    std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    // ------------------------------------------------------------------ screens

    void home(App& app)
    {
        ImGui::TextUnformatted("TEKKEN TAG TOURNAMENT 2  -  Native Online");
        ImGui::Spacing();
        if (!app.build)
        {
            ImGui::TextWrapped("First-time setup: your own copy of the game is verified and compiled on this PC. "
                               "Put your disc image (.iso) in the 'iso' folder, then choose Set up the game.");
            ImGui::Spacing();
            if (big_button("Set up the game"))
                app.screen = Screen::Setup;
        }
        else
        {
            ImGui::Text("Game: %s   build %s", app.build->version.c_str(), app.build->build_id.c_str());
            ImGui::Spacing();
            if (big_button("Local versus (2 controllers)"))
                start_game(app, name_args(app.config.name, "Player 2"));
#ifdef TT2_WITH_LOBBY
            if (big_button("Online lobby"))
                app.screen = Screen::Lobby;
#endif
            if (big_button("Direct connect (IP)"))
                app.screen = Screen::Direct;
            if (big_button("Settings"))
                app.screen = Screen::Settings;
            if (big_button("Rebuild the game (after an update)"))
                app.screen = Screen::Setup;
        }
        if (big_button("Quit"))
        {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }

    void setup(App& app)
    {
        ImGui::TextUnformatted("Set up the game");
        ImGui::Spacing();
        if (app.child && app.child->running())
        {
            ImGui::TextWrapped("Setup is running in its own window. It verifies your disc, copies the game data into "
                               "this folder and compiles the game. This takes a few minutes.");
            return;
        }
        if (app.child)
        {
            const int code = app.child->exit_code();
            app.child.reset();
            refresh_build(app);
            app.message = code == 0 ? "Setup finished." : "Setup failed - see native\\build.log.";
            app.screen = Screen::Home;
            return;
        }
        ImGui::TextWrapped("Setup looks for your disc image in the 'iso' folder first. If none is there it opens a "
                           "file picker. Nothing is installed on your system; everything stays in this folder.");
        ImGui::Spacing();
        if (big_button(app.build ? "Rebuild (keep verified game data)" : "Start setup"))
        {
            std::vector<std::wstring> args = { L"/c", (app.root / "Setup.bat").wstring() };
            if (app.build)
                args.push_back(L"-Rebuild");
            app.child = ChildProcess::start(L"cmd.exe", args, app.root, true);
        }
        if (big_button("Back"))
            app.screen = Screen::Home;
    }

    void direct(App& app)
    {
        ImGui::TextUnformatted("Direct connect");
        ImGui::TextWrapped("For LAN, port forwarding or a VPN. Both players pick opposite sides and enter each other's "
                           "address. UDP port %u must be reachable.", app.config.local_port);
        ImGui::Spacing();
        ImGui::Text("Peer address:  %s", app.config.direct_peer.empty() ? "(not set)" : app.config.direct_peer.c_str());
        if (big_button("Edit peer address"))
            app.osk.open("Peer address (ip:port)", app.config.direct_peer, Osk::Layout::Address, [&app](const std::string& v) {
                app.config.direct_peer = v;
                save_config(app.native / "launcher.json", app.config);
            });
        ImGui::Text("Your side:  Player %d", app.config.direct_side + 1);
        if (big_button("Switch side"))
            app.config.direct_side ^= 1;
        if (big_button("Connect", !app.config.direct_peer.empty()))
        {
            std::string host = app.config.direct_peer;
            std::string port = std::to_string(app.config.local_port);
            if (host.find(':') == std::string::npos)
                host += ":" + port;
            std::vector<std::wstring> args = { L"--netplay", L"--player", std::to_wstring(app.config.direct_side + 1), L"--local-port", widen(port),
                                               L"--peer", widen(host), L"--build-id", widen(app.build->build_id),
                                               L"--delay", std::to_wstring(app.config.input_delay) };
            // The other player's name is not known here: both PCs use "Player N" for it.
            const auto names = app.config.direct_side == 0 ? name_args(app.config.name, "Player 2") : name_args("Player 1", app.config.name);
            args.insert(args.end(), names.begin(), names.end());
            start_game(app, args);
        }
        if (big_button("Back"))
            app.screen = Screen::Home;
    }

#ifdef TT2_WITH_LOBBY
    void lobby(App& app)
    {
        using namespace tt2::lobby;
        if (!app.lobby)
        {
            LobbyConfig cfg;
            cfg.build_id = app.build->build_id;
            cfg.display_name = app.config.name;
            cfg.avatar_png = read_file_bytes(avatar_file(app));
            cfg.room = app.config.room;
            cfg.trackers = app.config.trackers;
            cfg.ice_servers = app.config.ice_servers;
            app.lobby = std::make_unique<Lobby>(cfg);
            app.lobby->start();
        }

        for (const auto& e : app.lobby->poll_events())
        {
            switch (e.type)
            {
            case EventType::ChallengeReceived:
                app.challenger = e.peer_id;
                break;
            case EventType::ChallengeDeclined:
                app.message = "Challenge declined.";
                break;
            case EventType::TrackerStatus:
                app.lobby_log = e.message;
                break;
            case EventType::MatchReady:
            {
                // The launcher relays the WebRTC match channel to the game over loopback.
                const uint16_t relay_port = uint16_t(app.config.local_port + 1);
                const uint16_t game_port = app.config.local_port;
                std::string error;
                app.relay = std::make_unique<LoopbackRelay>(app.lobby->take_match_transport(e.peer_id), relay_port, game_port, &error);
                if (!app.relay->ok())
                {
                    app.message = "Could not open the match connection: " + error;
                    app.relay.reset();
                    break;
                }
                app.lobby->set_status(PeerStatus::InMatch);
                std::string other = "Opponent";
                for (const auto& p : app.lobby->peers())
                    if (p.id == e.peer_id)
                        other = p.name;
                std::vector<std::wstring> args = { L"--netplay", L"--player", std::to_wstring(e.local_player + 1), L"--local-port", std::to_wstring(game_port),
                                                   L"--peer", L"127.0.0.1:" + std::to_wstring(relay_port), L"--build-id", widen(app.build->build_id),
                                                   L"--delay", std::to_wstring(app.config.input_delay) };
                const auto names = e.local_player == 0 ? name_args(app.config.name, other) : name_args(other, app.config.name);
                args.insert(args.end(), names.begin(), names.end());
                start_game(app, args);
                break;
            }
            default:
                break;
            }
        }

        avatar_image(app.avatar, 48);
        ImGui::SameLine();
        ImGui::Text("Online lobby  -  room '%s'  -  you are %s", app.config.room.c_str(), app.config.name.c_str());
        ImGui::TextDisabled("%s", app.lobby_log.c_str());
        ImGui::Spacing();

        if (!app.challenger.empty())
        {
            std::string name = app.challenger;
            for (auto& p : app.lobby->peers())
                if (p.id == app.challenger)
                    name = p.name;
            ImGui::Text("%s challenges you!", name.c_str());
            if (big_button("Accept"))
            {
                app.lobby->accept(app.challenger);
                app.challenger.clear();
            }
            if (big_button("Decline"))
            {
                app.lobby->decline(app.challenger);
                app.challenger.clear();
            }
            return;
        }

        const auto peers = app.lobby->peers();
        if (peers.empty())
            ImGui::TextWrapped("Looking for players with the same game build... Peers are found through public "
                               "WebTorrent trackers and connect to you directly (no lobby server).");
        for (const auto& p : peers)
        {
            // Their picture, made into a texture when it arrives or changes.
            auto& cached = app.peer_avatars[p.id];
            if (cached.first != p.avatar_png)
            {
                if (cached.second)
                    SDL_DestroyTexture(cached.second);
                cached = { p.avatar_png, nullptr };
                Rgba image;
                if (!p.avatar_png.empty() && decode_image_bytes(p.avatar_png, image))
                    cached.second = make_texture(app.renderer, image);
            }
            avatar_image(cached.second, 56);
            ImGui::SameLine();
            const char* status = p.status == PeerStatus::InMatch ? "in a match" : p.status == PeerStatus::Away ? "away" : "ready";
            char label[160];
            snprintf(label, sizeof(label), "%-24s  %4d ms   %s##%s", p.name.c_str(), p.ping_ms, status, p.id.c_str());
            if (big_button(label, p.status == PeerStatus::Idle))
            {
                app.lobby->challenge(p.id);
                app.message = "Challenge sent to " + p.name + ".";
            }
        }
        ImGui::Spacing();
        if (big_button("Leave lobby"))
        {
            app.lobby.reset();
            app.screen = Screen::Home;
        }
    }
#endif

    void settings(App& app)
    {
        ImGui::TextUnformatted("Settings");
        ImGui::Spacing();
        avatar_image(app.avatar, 96);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Name: %s", app.config.name.c_str());
        ImGui::TextDisabled("Your name and picture are shown to other players in the lobby.");
        ImGui::EndGroup();
        if (big_button("Choose profile picture..."))
        {
            const fs::path chosen = pick_image_file(SDL_GetPointerProperty(SDL_GetWindowProperties(app.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
            if (!chosen.empty())
            {
                std::string error;
                if (import_avatar(chosen, avatar_file(app), &error))
                {
                    reload_avatar(app);
                    app.message = "Profile picture saved.";
                }
                else
                    app.message = error;
            }
        }
        if (app.avatar && big_button("Remove profile picture"))
        {
            std::error_code ec;
            fs::remove(avatar_file(app), ec);
            reload_avatar(app);
        }
        if (big_button("Change name"))
            app.osk.open("Your name", app.config.name, Osk::Layout::Text, [&app](const std::string& v) {
                if (!v.empty())
                    app.config.name = v.substr(0, 24);
                save_config(app.native / "launcher.json", app.config);
            });
        ImGui::Text("Lobby room: %s", app.config.room.c_str());
        if (big_button("Change room (play with friends only)"))
            app.osk.open("Room name", app.config.room, Osk::Layout::Text, [&app](const std::string& v) {
                app.config.room = v.empty() ? "public" : v;
                save_config(app.native / "launcher.json", app.config);
            });
        if (big_button(app.config.fullscreen ? "Display: fullscreen" : "Display: window"))
            app.config.fullscreen = !app.config.fullscreen;
        char delay[64];
        if (app.config.input_delay < 0)
            snprintf(delay, sizeof(delay), "Input delay: automatic");
        else
            snprintf(delay, sizeof(delay), "Input delay: %d frame(s)", app.config.input_delay);
        if (big_button(delay))
            app.config.input_delay = app.config.input_delay >= 4 ? -1 : app.config.input_delay + 1;
        ImGui::Spacing();
        if (big_button("Save and go back"))
        {
            save_config(app.native / "launcher.json", app.config);
            app.screen = Screen::Home;
        }
    }

    void in_game(App& app)
    {
#ifdef TT2_WITH_LOBBY
        if (app.relay)
            app.relay->pump();
#endif
        if (app.child && app.child->running())
        {
            ImGui::TextUnformatted("The game is running.");
            return;
        }
        app.child.reset();
#ifdef TT2_WITH_LOBBY
        if (app.relay)
        {
            app.relay.reset();
            if (app.lobby)
                app.lobby->set_status(tt2::lobby::PeerStatus::Idle);
            app.screen = Screen::Lobby;
            return;
        }
#endif
        app.screen = Screen::Home;
    }
}

int main(int, char**)
{
    App app;
    app.root = executable_dir();
    // Development builds run from build\bin: walk up to the folder with Setup.bat.
    for (int i = 0; i < 4 && !fs::exists(app.root / "Setup.bat"); i++)
        app.root = app.root.parent_path();
    app.native = app.root / "native";
    std::error_code ec;
    fs::create_directories(app.native, ec);
    app.config = load_config(app.native / "launcher.json", app.root / ".env");
    refresh_build(app);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        return 1;
    SDL_Window* window = SDL_CreateWindow("TTT2 Native Online", 960, 640, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);
    app.window = window;
    app.renderer = renderer;
    reload_avatar(app);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.6f;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FrameRounding = 6;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT)
                running = false;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        // B / Escape goes back
        if (!app.osk.active() && app.screen != Screen::Home && app.screen != Screen::InGame &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
        {
#ifdef TT2_WITH_LOBBY
            if (app.screen == Screen::Lobby)
                app.lobby.reset();
#endif
            if (!(app.screen == Screen::Setup && app.child))
                app.screen = Screen::Home;
        }

        if (app.osk.active())
            app.osk.draw();
        else
        {
            switch (app.screen)
            {
            case Screen::Home: home(app); break;
            case Screen::Setup: setup(app); break;
            case Screen::Direct: direct(app); break;
            case Screen::Settings: settings(app); break;
            case Screen::InGame: in_game(app); break;
            case Screen::Lobby:
#ifdef TT2_WITH_LOBBY
                lobby(app);
#endif
                break;
            }
        }

        if (!app.message.empty())
        {
            ImGui::Separator();
            ImGui::TextWrapped("%s", app.message.c_str());
        }
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
        ImGui::TextDisabled("A select   B back   |   rollback netcode based on Tekken6-rollback by Ekspym");
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 10, 10, 14, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

#ifdef TT2_WITH_LOBBY
    app.relay.reset();
    app.lobby.reset();
#endif
    save_config(app.native / "launcher.json", app.config);
    if (app.avatar)
        SDL_DestroyTexture(app.avatar);
    for (auto& [id, cached] : app.peer_avatars)
        if (cached.second)
            SDL_DestroyTexture(cached.second);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
