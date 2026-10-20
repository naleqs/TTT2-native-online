#pragma once

#include <cstdint>

namespace tt2::audio
{
    bool init();
    void init_xma(); // after guest memory exists: the XMA context array and registers
    void shutdown();
    // Rollback re-simulation must not play sounds twice.
    void set_muted(bool muted);

    // Netplay: audio follows the game's frames instead of the wall clock. Called at every live
    // frame boundary on the main thread: renders the 800 samples (3.125 mixer ticks) of one
    // frame and does the XMA work for them. The game waits on sounds (intros end when the voice
    // line has played), so both players' games must hear time pass identically. The first call
    // stops the real-time driver.
    void frame_step();
    // Back to the real-time driver (outside battles: the movie player waits for audio mid-frame).
    void stop_frame_driven();

    // Each XAudio2 engine registers its own render driver client (the game and the movie
    // player both have one). Returns the driver handle, 0 if all slots are taken.
    uint32_t register_client(uint32_t callback, uint32_t arg);
    void unregister_client(uint32_t driver);
    void submit(uint32_t driver, uint32_t samples);
}
