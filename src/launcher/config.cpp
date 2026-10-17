#include "config.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>
#include <sstream>

namespace tt2::launcher
{
    namespace
    {
        std::map<std::string, std::string> read_dotenv(const std::filesystem::path& path)
        {
            std::map<std::string, std::string> out;
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line))
            {
                const auto start = line.find_first_not_of(" \t");
                if (start == std::string::npos || line[start] == '#')
                    continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                auto trim = [](std::string s) {
                    s.erase(0, s.find_first_not_of(" \t\"\r"));
                    s.erase(s.find_last_not_of(" \t\"\r") + 1);
                    return s;
                };
                out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
            // Real environment variables win over the file.
            for (auto& [k, v] : out)
                if (const char* e = std::getenv(k.c_str()))
                    v = e;
            return out;
        }

        std::vector<std::string> split_list(const std::string& s)
        {
            std::vector<std::string> out;
            std::stringstream ss(s);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                item.erase(0, item.find_first_not_of(' '));
                item.erase(item.find_last_not_of(' ') + 1);
                if (!item.empty())
                    out.push_back(item);
            }
            return out;
        }
    }

    Config load_config(const std::filesystem::path& file, const std::filesystem::path& dotenv)
    {
        Config c;
        const auto env = read_dotenv(dotenv);
        auto get = [&](const char* key) -> std::string {
            if (auto it = env.find(key); it != env.end())
                return it->second;
            const char* e = std::getenv(key);
            return e ? e : "";
        };
        if (auto v = get("TT2_PLAYER_NAME"); !v.empty()) c.name = v;
        if (auto v = get("TT2_LOBBY_ROOM"); !v.empty()) c.room = v;
        if (auto v = get("TT2_NETPLAY_PORT"); !v.empty()) c.local_port = unsigned(std::stoul(v));
        if (auto v = get("TT2_INPUT_DELAY"); !v.empty()) c.input_delay = v == "auto" ? -1 : std::stoi(v);
        c.trackers = split_list(get("TT2_TRACKERS"));
        c.ice_servers = split_list(get("TT2_ICE_SERVERS"));

        std::ifstream f(file);
        if (f)
        {
            const auto j = nlohmann::json::parse(f, nullptr, false);
            if (!j.is_discarded())
            {
                c.name = j.value("name", c.name);
                c.room = j.value("room", c.room);
                c.fullscreen = j.value("fullscreen", c.fullscreen);
                c.input_delay = j.value("input_delay", c.input_delay);
                c.local_port = j.value("local_port", c.local_port);
                c.direct_peer = j.value("direct_peer", c.direct_peer);
                c.direct_side = j.value("direct_side", c.direct_side) & 1;
            }
        }
        return c;
    }

    void save_config(const std::filesystem::path& file, const Config& c)
    {
        nlohmann::json j = {
            { "name", c.name },
            { "room", c.room },
            { "fullscreen", c.fullscreen },
            { "input_delay", c.input_delay },
            { "local_port", c.local_port },
            { "direct_peer", c.direct_peer },
            { "direct_side", c.direct_side },
        };
        std::ofstream(file) << j.dump(2);
    }

    std::optional<BuildInfo> read_build_info(const std::filesystem::path& file)
    {
        std::ifstream f(file);
        if (!f)
            return std::nullopt;
        // setup.ps1 writes this with Set-Content -Encoding UTF8, i.e. with a BOM.
        std::string text((std::istreambuf_iterator<char>(f)), {});
        if (text.rfind("\xEF\xBB\xBF", 0) == 0)
            text.erase(0, 3);
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.contains("build_id"))
            return std::nullopt;
        return BuildInfo{ j.value("version", ""), j.value("build_id", "") };
    }
}
