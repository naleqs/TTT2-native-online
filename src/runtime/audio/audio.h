#pragma once

#include <cstdint>

namespace tt2::audio
{
    bool init();
    void shutdown();
    // Rollback re-simulation must not play sounds twice.
    void set_muted(bool muted);

    void register_client(uint32_t callback, uint32_t arg);
    void unregister_client();
    void submit(uint32_t samples);
}
