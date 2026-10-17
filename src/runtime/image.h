#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace tt2
{
    // tt2_image.json written by tt2-analyze at setup time.
    struct ImageInfo
    {
        uint32_t base = 0;
        uint32_t size = 0;
        uint32_t entry = 0;
        uint32_t stack_size = 0x40000;
        uint32_t tls_slots = 0;
        uint32_t tls_raw_address = 0;
        uint32_t tls_data_size = 0;
        uint32_t tls_raw_size = 0;
    };

    extern ImageInfo g_image;
    // XEX optional headers: key -> inline value, or guest address of a copy of the data.
    extern std::map<uint32_t, uint32_t> g_xex_headers;

    // Loads tt2_image.bin into guest memory and fills the data imports.
    bool load_image(const std::filesystem::path& dir, std::string& error);
}
