#pragma once

// Direct3D 11 renderer for the Xenos command stream. Everything except init() is called on the
// GPU thread (video.cpp's command processor).
//
// EDRAM is modelled as one host render target per (EDRAM base, format, pitch). A resolve copies
// a render target rectangle into a host texture kept for the destination address, which later
// texture fetches of that address and the front buffer use directly. Resolved data reaches
// guest memory only when the CPU reads it through the 0xC0000000 view (see write_back_fault).

#include <cstdint>
#include <filesystem>

namespace tt2::gpu::renderer
{
    // Creates the device and swap chain for the window. False: stay with the placeholder.
    bool init(void* hwnd, const std::filesystem::path& screenshot_dir);
    bool active();

    void load_shader(uint32_t type, const uint32_t* words_be, uint32_t count); // IM_LOAD(_IMMEDIATE)
    // DRAW_INDX(_2). index_base/index_size: the DMA index buffer (physical address, VGT_DMA_SIZE).
    void draw(uint32_t initiator, uint32_t index_base, uint32_t index_size);
    void swap(uint32_t frontbuffer_physical, uint32_t width, uint32_t height);
}
