#pragma once

// Translates Xenos shader microcode (as loaded with IM_LOAD) into HLSL for Direct3D 11.
//
// The instruction formats follow Xenia's ucode.h (BSD-3-Clause). Resource layout of the
// generated code, shared with the renderer:
//
//   b0  cbuffer FloatConsts { float4 c[256]; }        the stage's 256 ALU constants
//   b1  cbuffer IntConsts   { uint4 bools[2]; uint4 loops[8]; }  (each stage's own half)
//   b2  cbuffer DrawConsts  { uint4 vfetch[48]; ... }   see kDrawConstsHlsl in the .cpp
//   t0-t31, s0-s31          texture fetch constants tf0-tf31
//   t32                     ByteAddressBuffer with the draw's vertex data (vertex shaders)
//
// Vertex shaders output SV_Position and 16 interpolators (TEXCOORD0-15) plus the point size
// (TEXCOORD16); pixel shaders take the same layout and write SV_Target0-3 / SV_Depth.

#include <cstdint>
#include <string>
#include <vector>

namespace tt2::gpu
{
    enum class ShaderStage { Vertex, Pixel };

    struct ShaderTexture
    {
        uint32_t slot;      // tf#
        uint32_t dimension; // 0 1D, 1 2D, 2 3D/stacked, 3 cube
    };

    struct TranslatedShader
    {
        ShaderStage stage = ShaderStage::Vertex;
        std::string hlsl;
        std::vector<uint32_t> vertex_slots;   // vf# fetch constants the shader reads
        // Per entry of vertex_slots: the largest stride and the furthest dword one vertex reaches
        // (offset + element size), so only the vertices a draw uses need uploading.
        std::vector<uint32_t> vertex_strides, vertex_extents;
        std::vector<ShaderTexture> textures;
        uint32_t color_targets = 0;           // pixel shaders: bit per oC#
        bool writes_depth = false;
        bool kills = false;
        bool memexport = false;               // uses eA/eM (not supported, ignored)
        bool ok = false;
        std::string error;
    };

    // words are the shader's dwords as they are in guest memory (big-endian). no_gradients
    // samples textures at an explicit LOD, for shaders whose flow control the HLSL compiler
    // rejects implicit derivatives in.
    TranslatedShader translate_shader(ShaderStage stage, const uint32_t* words_be, uint32_t count, bool no_gradients = false);
}
