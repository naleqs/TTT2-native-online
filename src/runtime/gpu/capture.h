#pragma once

// What the game asks the GPU to do, frame by frame: draws, resolves (EDRAM -> memory copies),
// render target setups and the shader programs it loads. The renderer is built against this;
// until it exists, the statistics (TT2_GPU_STATS=1) and the dumped shader microcode
// (TT2_DUMP_SHADERS=1, native\gpu\shaders) show what it has to support.

#include <cstdint>
#include <filesystem>

namespace tt2::gpu
{
    // Register file of the command processor (the guest's MMIO window at 0x7FC80000).
    uint32_t reg(uint32_t index);

    void on_shader_load(uint32_t type, const uint32_t* words_be, uint32_t count); // IM_LOAD
    void on_draw(uint32_t initiator);                                             // DRAW_INDX(_2)
    void on_frame_end();                                                         // the swap packet
    void configure();                                                             // reads the environment
    void set_shader_dump_dir(const std::filesystem::path& dir);
}
