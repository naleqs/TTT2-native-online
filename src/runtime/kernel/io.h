#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tt2::io
{
    struct Resolved
    {
        std::filesystem::path host;
        bool read_only = false;
    };

    // Maps an Xbox device (e.g. "\Device\Cdrom0") onto a host folder.
    void mount(const std::string& device, const std::filesystem::path& host, bool read_only);
    // "game:" -> "\Device\Cdrom0"
    void link(const std::string& name, const std::string& target);
    void unlink(const std::string& name);

    std::optional<Resolved> resolve(std::string guest_path);

    std::string read_ansi_string(uint32_t ansi_string);
    std::string object_attributes_path(uint32_t object_attributes);
}
