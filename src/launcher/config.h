#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tt2::launcher
{
    // native\launcher.json, with defaults from .env (see .env.example).
    struct Config
    {
        std::string name = "Player";
        std::string room = "public";
        bool fullscreen = false;
        int input_delay = -1;           // -1 = automatic
        unsigned local_port = 7000;
        std::string direct_peer;        // "ip:port"
        int direct_side = 0;
        std::vector<std::string> trackers;
        std::vector<std::string> ice_servers;
    };

    struct BuildInfo
    {
        std::string version;
        std::string build_id;
    };

    Config load_config(const std::filesystem::path& file, const std::filesystem::path& dotenv);
    void save_config(const std::filesystem::path& file, const Config& config);
    std::optional<BuildInfo> read_build_info(const std::filesystem::path& file);
}
