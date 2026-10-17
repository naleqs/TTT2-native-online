#pragma once

#include <cstdint>

namespace tt2::video
{
    void start();
    void stop();
    uint64_t frames(); // VdSwap count
}
