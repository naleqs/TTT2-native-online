#pragma once

// Profile pictures: chosen from any image file, cropped to a square and stored as a small PNG
// (native\profile\avatar.png) that the lobby sends to other players. Decoding and encoding use
// the Windows Imaging Component, which is part of Windows.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tt2::launcher
{
    constexpr int kAvatarSize = 96;

    struct Rgba
    {
        int width = 0, height = 0;
        std::vector<uint8_t> pixels; // width * height * 4, R G B A
    };

    // Opens the system's file picker for an image. Empty when cancelled.
    std::filesystem::path pick_image_file(void* owner_hwnd);

    // Reads `source` (PNG, JPEG, BMP, GIF, ...), crops it to a centred square, scales it to
    // kAvatarSize and writes a PNG to `destination`.
    bool import_avatar(const std::filesystem::path& source, const std::filesystem::path& destination, std::string* error);

    // Decodes an image file or in-memory image bytes (another player's PNG) to RGBA.
    bool decode_image_file(const std::filesystem::path& file, Rgba& out);
    bool decode_image_bytes(const std::string& bytes, Rgba& out);

    std::string read_file_bytes(const std::filesystem::path& file);
}
