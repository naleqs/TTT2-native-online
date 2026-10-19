#include "capture.h"
#include "../guest.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace tt2::gpu
{
    namespace
    {
        // Register indices (Xenia's register_table.inc)
        constexpr uint32_t RB_SURFACE_INFO = 0x2000;
        constexpr uint32_t RB_COLOR_INFO = 0x2001;
        constexpr uint32_t RB_DEPTH_INFO = 0x2002;
        constexpr uint32_t RB_MODECONTROL = 0x2208;
        constexpr uint32_t RB_COPY_CONTROL = 0x2318;
        constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;

        constexpr uint32_t kEdramCopy = 6; // RB_MODECONTROL edram_mode

        bool g_stats = false;
        bool g_dump_shaders = false;
        fs::path g_shader_dir;

        struct FrameStats
        {
            uint32_t draws = 0;
            uint32_t resolves = 0;
            uint64_t indices = 0;
            std::array<uint32_t, 32> prims{};
            std::set<uint32_t> color_formats;
        };
        FrameStats g_frame;
        uint64_t g_frames = 0;
        std::map<uint64_t, uint32_t> g_shaders; // hash -> type
        uint32_t g_vertex_shaders = 0, g_pixel_shaders = 0;

        uint64_t fnv(const uint32_t* words, uint32_t count)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < count; i++)
                h = (h ^ words[i]) * 1099511628211ull;
            return h;
        }

        const char* prim_name(uint32_t p)
        {
            switch (p)
            {
            case 0x01: return "points";
            case 0x02: return "lines";
            case 0x03: return "line-strip";
            case 0x04: return "triangles";
            case 0x05: return "fan";
            case 0x06: return "strip";
            case 0x08: return "rects";
            case 0x0D: return "quads";
            default: return "other";
            }
        }
    }

    void configure()
    {
        auto on = [](const char* name) {
            const char* v = std::getenv(name);
            return v && *v == '1';
        };
        g_stats = on("TT2_GPU_STATS");
        g_dump_shaders = on("TT2_DUMP_SHADERS");
    }

    uint32_t reg(uint32_t index) { return load32(0x7FC80000 + index * 4); }

    void set_shader_dump_dir(const fs::path& dir)
    {
        g_shader_dir = dir;
    }

    void on_shader_load(uint32_t type, const uint32_t* words_be, uint32_t count)
    {
        const uint64_t h = fnv(words_be, count);
        if (!g_shaders.emplace(h, type).second)
            return;
        (type == 0 ? g_vertex_shaders : g_pixel_shaders)++;
        if (g_dump_shaders && !g_shader_dir.empty())
        {
            std::error_code ec;
            fs::create_directories(g_shader_dir, ec);
            char name[64];
            snprintf(name, sizeof(name), "%s_%016llx.bin", type == 0 ? "vs" : "ps", (unsigned long long)h);
            std::ofstream(g_shader_dir / name, std::ios::binary).write(reinterpret_cast<const char*>(words_be), std::streamsize(count) * 4);
        }
    }

    void on_draw(uint32_t initiator)
    {
        // VGT_DRAW_INITIATOR: prim_type 0-5, source_select 6-7, major_mode 8-9, index_size 11,
        // num_indices 16-31
        const uint32_t prim = initiator & 0x3F;
        const uint32_t indices = initiator >> 16;
        if ((reg(RB_MODECONTROL) & 7) == kEdramCopy)
        {
            g_frame.resolves++; // a "draw" in copy mode resolves EDRAM to memory
            return;
        }
        g_frame.draws++;
        g_frame.indices += indices;
        g_frame.prims[prim & 31]++;
        g_frame.color_formats.insert((reg(RB_COLOR_INFO) >> 16) & 0xF);
    }

    void on_frame_end()
    {
        g_frames++;
        if (g_stats && g_frames % 120 == 0)
        {
            std::string prims;
            for (uint32_t p = 0; p < 32; p++)
                if (g_frame.prims[p])
                    prims += std::string(" ") + prim_name(p) + "=" + std::to_string(g_frame.prims[p]);
            std::string formats;
            for (uint32_t f : g_frame.color_formats)
                formats += " " + std::to_string(f);
            log("gpu: frame %llu: %u draws (%llu indices), %u resolves, prims:%s, color formats:%s, shaders so far: %u VS / %u PS, surface %08X depth %08X copy %08X/%08X",
                (unsigned long long)g_frames, g_frame.draws, (unsigned long long)g_frame.indices, g_frame.resolves, prims.c_str(), formats.c_str(),
                g_vertex_shaders, g_pixel_shaders, reg(RB_SURFACE_INFO), reg(RB_DEPTH_INFO), reg(RB_COPY_CONTROL), reg(RB_COPY_DEST_INFO));
        }
        g_frame = FrameStats{};
    }
}
