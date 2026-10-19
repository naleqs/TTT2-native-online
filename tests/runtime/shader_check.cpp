// Translates dumped Xenos shaders (TT2_DUMP_SHADERS=1 writes them to native\gpu\shaders) to
// HLSL and compiles them with d3dcompiler_47.dll, which ships with Windows.
//
//   tt2_shader_check <dir> [--write]    --write keeps the .hlsl next to each shader

#include "gpu/shader_translator.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tt2::gpu;

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <shader dir> [--write]\n", argv[0]);
        return 2;
    }
    const bool write = argc > 2 && std::string(argv[2]) == "--write";
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!compile)
    {
        std::fprintf(stderr, "d3dcompiler_47.dll not found\n");
        return 2;
    }

    int total = 0, translated = 0, compiled = 0;
    for (const auto& e : fs::directory_iterator(argv[1]))
    {
        const std::string name = e.path().filename().string();
        if (e.path().extension() != ".bin" || name.size() < 3)
            continue;
        const ShaderStage stage = name.rfind("vs_", 0) == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        total++;

        bool no_gradients = false;
        for (int attempt = 0; attempt < 2; attempt++)
        {
            TranslatedShader t = translate_shader(stage, words.data(), uint32_t(words.size()), no_gradients);
            if (!t.ok)
            {
                std::printf("%s: translation failed: %s\n", name.c_str(), t.error.c_str());
                break;
            }
            if (attempt == 0)
                translated++;
            if (write)
                std::ofstream(fs::path(e.path()).replace_extension(".hlsl")) << t.hlsl;
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            const HRESULT hr = compile(t.hlsl.data(), t.hlsl.size(), name.c_str(), nullptr, nullptr, "main",
                                       stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
            const std::string err = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
            if (errors)
                errors->Release();
            if (SUCCEEDED(hr))
            {
                compiled++;
                std::printf("%s: ok (%zu bytes of DXBC%s%s%s, %zu textures, %zu vertex fetch slots)\n", name.c_str(),
                            size_t(code->GetBufferSize()), no_gradients ? ", explicit LOD" : "", t.kills ? ", kills" : "",
                            t.memexport ? ", memexport" : "", t.textures.size(), t.vertex_slots.size());
                code->Release();
                break;
            }
            if (attempt == 1 || stage == ShaderStage::Vertex)
            {
                std::printf("%s: compile failed:\n%s\n", name.c_str(), err.c_str());
                break;
            }
            no_gradients = true; // retry with explicit LOD sampling
        }
    }
    std::printf("%d shaders, %d translated, %d compiled\n", total, translated, compiled);
    return compiled == total ? 0 : 1;
}
