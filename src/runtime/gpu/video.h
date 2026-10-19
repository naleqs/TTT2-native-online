#pragma once

#include <cstdint>
#include <utility>

namespace tt2::video
{
    void start();
    void stop();
    uint64_t frames(); // VdSwap count
    // Guest address and size in bytes of the GPU ring buffer (0, 0 before it exists).
    std::pair<uint32_t, uint32_t> ring_range();
}
