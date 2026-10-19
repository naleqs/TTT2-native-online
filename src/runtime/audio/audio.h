#pragma once

#include <cstdint>

namespace tt2::audio
{
    bool init();
    void init_xma(); // after guest memory exists: the XMA context array and registers
    void shutdown();
    // Rollback re-simulation must not play sounds twice.
    void set_muted(bool muted);

    // Each XAudio2 engine registers its own render driver client (the game and the movie
    // player both have one). Returns the driver handle, 0 if all slots are taken.
    uint32_t register_client(uint32_t callback, uint32_t arg);
    void unregister_client(uint32_t driver);
    void submit(uint32_t driver, uint32_t samples);
}
