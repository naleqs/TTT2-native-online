#pragma once

// Checks GitHub once, in the background, for a newer release of this project.

#include <optional>
#include <string>

namespace tt2::launcher
{
    struct UpdateInfo
    {
        std::string version; // the newer release ("0.2.0" for a tag "v0.2.0")
        std::string url;     // its release page
    };

    // Starts the check (a background thread; does nothing if the check is already running).
    void start_update_check(const std::string& current_version);
    // A newer release, once the check has found one.
    std::optional<UpdateInfo> available_update();
    // True when `latest` is a higher dotted version than `current` ("0.2.0" > "0.1.9").
    bool is_newer_version(const std::string& latest, const std::string& current);
}
