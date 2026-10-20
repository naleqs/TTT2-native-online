#include "renderer.h"
#include "capture.h"
#include "shader_translator.h"
#include "../guest.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <tuple>
#include <chrono>
#include <thread>
#include <mutex>
#include <iterator>
#include <future>
#include <functional>
#include <deque>
#include <condition_variable>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace tt2::gpu::renderer
{
    namespace
    {
        // ------------------------------------------------------------------ registers

        constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
        constexpr uint32_t RB_COLOR_INFO_REGS[4] = { 0x2001, 0x2003, 0x2004, 0x2005 };
        constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082;
        constexpr uint32_t VGT_INDX_OFFSET = 0x2102, RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105;
        constexpr uint32_t RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E, PA_CL_VPORT_XSCALE = 0x210F;
        constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180, SQ_CONTEXT_MISC = 0x2181;
        constexpr uint32_t RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202;
        constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208;
        constexpr uint32_t RB_BLENDCONTROL_REGS[4] = { 0x2201, 0x2209, 0x220A, 0x220B };
        constexpr uint32_t PA_SU_POINT_SIZE = 0x2280, PA_SU_VTX_CNTL = 0x2302, SQ_VS_CONST = 0x2307, SQ_PS_CONST = 0x2308;
        constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B;
        constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E;
        constexpr uint32_t kAluConsts = 0x4000, kFetchConsts = 0x4800, kBoolConsts = 0x4900, kLoopConsts = 0x4908;

        constexpr uint32_t kRtHeight = 2048;

        uint32_t r(uint32_t index) { return gpu::reg(index); }
        float rf(uint32_t index)
        {
            const uint32_t v = r(index);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }
        uint32_t guest_from_physical(uint32_t p) { return p >= 0x20000000 ? p : (p | 0xA0000000); }
        const uint8_t* phys_ptr(uint32_t p) { return g_base + guest_from_physical(p); }

        uint32_t gpu_swap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
            case 2: return __builtin_bswap32(v);
            case 3: return (v << 16) | (v >> 16);
            default: return v;
            }
        }

        uint64_t fnv(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull)
        {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            size_t i = 0;
            for (; i + 8 <= bytes; i += 8)
            {
                uint64_t v;
                std::memcpy(&v, p + i, 8);
                h = (h ^ v) * 1099511628211ull;
            }
            for (; i < bytes; i++)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        void warn_once(const std::string& what)
        {
            static std::mutex mutex;
            static std::set<std::string> seen;
            std::lock_guard lock(mutex);
            if (seen.insert(what).second)
                log("renderer: %s", what.c_str());
        }

        // ------------------------------------------------------------------ device

        using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                              UINT, ID3DBlob**, ID3DBlob**);

        bool g_active = false;
        HWND g_hwnd = nullptr;
        fs::path g_screenshot_dir;
        uint32_t g_screenshot_interval = 0;
        ComPtr<ID3D11Device> g_device;
        ComPtr<ID3D11Device1> g_device1;
        ComPtr<ID3D11DeviceContext> g_ctx;
        ComPtr<ID3D11DeviceContext1> g_ctx1;
        ComPtr<IDXGISwapChain1> g_swapchain;
        uint32_t g_backbuffer_w = 0, g_backbuffer_h = 0;
        bool g_tearing = false; // swap chain created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        D3DCompileFn g_compile = nullptr;

        constexpr UINT kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL1;

        ComPtr<ID3DBlob> compile(const std::string& src, const char* entry, const char* target, std::string* error = nullptr)
        {
            ComPtr<ID3DBlob> code, errors;
            const HRESULT hr = g_compile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, target, kCompileFlags, 0, &code, &errors);
            if (FAILED(hr))
            {
                if (error)
                    *error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?";
                return nullptr;
            }
            return code;
        }

        // ------------------------------------------------------------------ dynamic buffers

        // A ring in a dynamic buffer, mapped with NO_OVERWRITE and discarded when it wraps.
        struct Ring
        {
            ComPtr<ID3D11Buffer> buffer;
            ComPtr<ID3D11ShaderResourceView> srv; // raw view (vertex data)
            uint32_t size = 0, pos = 0;

            void create(uint32_t bytes, UINT bind, bool raw)
            {
                size = bytes;
                D3D11_BUFFER_DESC d{};
                d.ByteWidth = bytes;
                d.Usage = D3D11_USAGE_DYNAMIC;
                d.BindFlags = bind;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                d.MiscFlags = raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0;
                g_device->CreateBuffer(&d, nullptr, &buffer);
                if (raw)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_R32_TYPELESS;
                    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
                    v.BufferEx.NumElements = bytes / 4;
                    v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                    g_device->CreateShaderResourceView(buffer.Get(), &v, &srv);
                }
            }
            // Returns the offset of the copy, or ~0u if it does not fit.
            uint32_t write(const void* data, uint32_t bytes, void (*fill)(void* dst, const void* src, uint32_t bytes) = nullptr)
            {
                bytes = (bytes + 15) & ~15u;
                if (bytes > size)
                    return ~0u;
                D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
                if (pos + bytes > size)
                {
                    pos = 0;
                    mode = D3D11_MAP_WRITE_DISCARD;
                }
                D3D11_MAPPED_SUBRESOURCE m;
                if (FAILED(g_ctx->Map(buffer.Get(), 0, mode, 0, &m)))
                    return ~0u;
                uint8_t* dst = static_cast<uint8_t*>(m.pData) + pos;
                if (fill)
                    fill(dst, data, bytes);
                else
                    std::memcpy(dst, data, bytes);
                g_ctx->Unmap(buffer.Get(), 0);
                const uint32_t at = pos;
                pos += bytes;
                return at;
            }
        };
        Ring g_vertex_ring, g_index_ring;

        struct DrawConsts
        {
            uint32_t vfetch[48][4];
            float ndc_scale[4];
            float ndc_offset[4];
            uint32_t draw_flags[4];
            float alpha_ref[4];
            uint32_t tex_swizzle[8][4];
            float point_size[4];
        };
        struct IntConsts
        {
            uint32_t bools[8];
            uint32_t loops[32];
        };
        ComPtr<ID3D11Buffer> g_cb_vs_float, g_cb_ps_float, g_cb_int, g_cb_draw;

        ComPtr<ID3D11Buffer> make_cbuffer(uint32_t bytes)
        {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = (bytes + 15) & ~15u;
            d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> b;
            g_device->CreateBuffer(&d, nullptr, &b);
            return b;
        }
        void update_cbuffer(ID3D11Buffer* b, const void* data, size_t bytes)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                std::memcpy(m.pData, data, bytes);
                g_ctx->Unmap(b, 0);
            }
        }

        // ------------------------------------------------------------------ shaders

        // Shaders are translated and compiled on worker threads as soon as the game loads them
        // (IM_LOAD), which it mostly does during its own loading screens; a draw only waits for
        // a shader that is not ready yet. Compiled code is kept in native\cache\shaders, keyed
        // by the HLSL, so later runs skip the compiler (about 100 ms per shader).
        struct Shader
        {
            ShaderStage stage;
            std::vector<uint32_t> words;
            std::shared_future<bool> ready; // set by the compile task; meta, vs and ps after it
            TranslatedShader meta;
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
        };
        std::unordered_map<uint64_t, std::unique_ptr<Shader>> g_shaders;
        Shader* g_current_vs = nullptr;
        Shader* g_current_ps = nullptr;
        fs::path g_shader_cache;

        // Worker pool for shader compiles.
        std::mutex g_jobs_mutex;
        std::condition_variable g_jobs_cv;
        std::deque<std::function<void()>> g_jobs;
        std::vector<std::thread> g_workers;

        void start_workers()
        {
            const unsigned n = std::max(2u, std::thread::hardware_concurrency() > 4 ? std::thread::hardware_concurrency() - 3 : 2u);
            for (unsigned i = 0; i < n; i++)
                g_workers.emplace_back([] {
                    for (;;)
                    {
                        std::function<void()> job;
                        {
                            std::unique_lock lock(g_jobs_mutex);
                            g_jobs_cv.wait(lock, [] { return !g_jobs.empty(); });
                            job = std::move(g_jobs.front());
                            g_jobs.pop_front();
                        }
                        job();
                    }
                });
            for (auto& t : g_workers)
                t.detach();
        }

        std::vector<uint8_t> compile_cached(const std::string& hlsl, const char* target, std::string* error)
        {
            const uint64_t key = fnv(hlsl.data(), hlsl.size(), fnv(target, std::strlen(target), kCompileFlags));
            char name[40];
            snprintf(name, sizeof(name), "%016llx.dxbc", (unsigned long long)key);
            const fs::path path = g_shader_cache / name;
            std::error_code ec;
            if (!g_shader_cache.empty() && fs::exists(path, ec))
            {
                std::ifstream f(path, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
                if (!bytes.empty())
                    return bytes;
            }
            ComPtr<ID3DBlob> code = compile(hlsl, "main", target, error);
            if (!code)
                return {};
            const auto* data = static_cast<const uint8_t*>(code->GetBufferPointer());
            std::vector<uint8_t> bytes(data, data + code->GetBufferSize());
            if (!g_shader_cache.empty())
            {
                fs::create_directories(g_shader_cache, ec);
                const fs::path tmp = path.string() + ".tmp" + std::to_string(GetCurrentThreadId());
                std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                fs::rename(tmp, path, ec);
                if (ec)
                    fs::remove(tmp, ec);
            }
            return bytes;
        }

        // Runs on a worker: translate (retrying with explicit-LOD sampling for pixel shaders the
        // compiler rejects) and create the shader object (the device is free-threaded).
        bool build_shader(Shader& s)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                s.meta = translate_shader(s.stage, s.words.data(), uint32_t(s.words.size()), attempt == 1);
                if (!s.meta.ok)
                {
                    warn_once("shader translation failed: " + s.meta.error);
                    return false;
                }
                std::string error;
                const std::vector<uint8_t> code = compile_cached(s.meta.hlsl, s.stage == ShaderStage::Vertex ? "vs_5_0" : "ps_5_0", &error);
                if (code.empty())
                {
                    if (attempt == 0 && s.stage == ShaderStage::Pixel)
                        continue;
                    warn_once("shader compile failed: " + error.substr(0, 300));
                    return false;
                }
                if (s.stage == ShaderStage::Vertex)
                    g_device->CreateVertexShader(code.data(), code.size(), nullptr, &s.vs);
                else
                    g_device->CreatePixelShader(code.data(), code.size(), nullptr, &s.ps);
                return true;
            }
            return false;
        }

        void start_compile(Shader* s)
        {
            auto task = std::make_shared<std::packaged_task<bool()>>([s] { return build_shader(*s); });
            s->ready = task->get_future().share();
            {
                std::lock_guard lock(g_jobs_mutex);
                g_jobs.emplace_back([task] { (*task)(); });
            }
            g_jobs_cv.notify_one();
        }

        bool prepare(Shader& s)
        {
            if (!s.ready.valid())
                start_compile(&s);
            return s.ready.get(); // waits only if the compile is still running
        }

        // Geometry shaders for the primitive types Direct3D 11 does not have, and the present blit.
        const char kFixedHlsl[] = R"(
cbuffer DrawConsts : register(b2)
{
    uint4 vfetch[48];
    float4 ndc_scale;
    float4 ndc_offset;
    uint4 draw_flags;
    float4 alpha_ref;
    uint4 tex_swizzle[8];
    float4 point_size;
};
struct V { float4 pos : SV_Position; float4 t[16] : TEXCOORD0; float4 pts : TEXCOORD16; };

V combine(V a, V b, V c) // a + b - c
{
    V d;
    d.pos = a.pos + b.pos - c.pos;
    [unroll] for (uint i = 0; i < 16; i++) d.t[i] = a.t[i] + b.t[i] - c.t[i];
    d.pts = a.pts + b.pts - c.pts;
    return d;
}

[maxvertexcount(4)]
void gs_rect(triangle V v[3], inout TriangleStream<V> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d20 = dot(p2 - p0, p2 - p0);
    uint a, b, c;
    if (d01 >= d12 && d01 >= d20) { a = 0; b = 1; c = 2; }
    else if (d12 >= d20) { a = 1; b = 2; c = 0; }
    else { a = 2; b = 0; c = 1; }
    s.Append(v[c]);
    s.Append(v[a]);
    s.Append(v[b]);
    s.Append(combine(v[a], v[b], v[c]));
}

[maxvertexcount(4)]
void gs_quad(lineadj V v[4], inout TriangleStream<V> s)
{
    s.Append(v[0]);
    s.Append(v[1]);
    s.Append(v[3]);
    s.Append(v[2]);
}

[maxvertexcount(4)]
void gs_point(point V v[1], inout TriangleStream<V> s)
{
    float2 half_px = v[0].pts.x > 0.0f ? v[0].pts.xx * 0.5f : point_size.xy;
    float2 h = half_px * point_size.zw * v[0].pos.w;
    const float2 corners[4] = { float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1) };
    [unroll] for (uint i = 0; i < 4; i++)
    {
        V o = v[0];
        o.pos.xy += corners[i] * h;
        o.pts.zw = corners[i] * float2(0.5f, -0.5f) + 0.5f;
        s.Append(o);
    }
}

struct BlitOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
BlitOut vs_blit(uint id : SV_VertexID)
{
    BlitOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D blit_tex : register(t0);
SamplerState blit_smp : register(s0);
float4 ps_blit(BlitOut i) : SV_Target { return float4(blit_tex.Sample(blit_smp, i.uv).rgb, 1.0f); }
// Resolves drawn as a pass: destination pixel -> source sample (pixel * samples per pixel).
cbuffer CopyConsts : register(b3) { uint4 copy_scale; };
Texture2D<float> depth_src : register(t0);
float ps_depth_copy(BlitOut i) : SV_Target { return depth_src.Load(int3(uint2(i.pos.xy) * copy_scale.xy, 0)); }
// Colour resolve from a multisampled (sample-resolution) surface: sample 0 of each pixel.
Texture2D color_src : register(t0);
float4 ps_color_copy(BlitOut i) : SV_Target { return color_src.Load(int3(uint2(i.pos.xy) * copy_scale.xy, 0)); }
// EDRAM aliasing: the pixels of this surface that lie in EDRAM tiles another surface wrote,
// fetched from that surface. A tile is tile_width x 16 samples; a surface's row of tiles is
// pitch / tile_width tiles. edram: this surface's first tile and tiles per row, the other's;
// edram2: tile width, the other surface's used rows.
cbuffer EdramConsts : register(b5) { uint4 edram; uint4 edram2; };
float4 ps_edram_copy(BlitOut i) : SV_Target
{
    const uint2 p = uint2(i.pos.xy);
    const uint tw = edram2.x;
    const uint tile = edram.x + (p.y / 16) * edram.y + p.x / tw;
    if (tile < edram.z || tile >= 2048)
        discard;
    const uint rel = tile - edram.z;
    const uint2 q = uint2((rel % edram.w) * tw + p.x % tw, (rel / edram.w) * 16 + p.y % 16);
    if (q.y >= edram2.y)
        discard;
    return color_src.Load(int3(q, 0));
}
// FPS overlay: up to 8 characters of a 3x5 font (4 bits each in text.x, count in text.y) in a
// dark box at pixel text.zw, text_scale.x pixels per font cell.
cbuffer TextConsts : register(b4) { uint4 text; uint4 text_scale; };
static const uint kGlyphs[14] = { 31599, 11415, 29671, 29647, 23497, 31183, 31215, 29257, 31727, 31695,
                                  31140, 31716, 31183, 0 }; // 0-9 F P S space
float4 ps_text(BlitOut i) : SV_Target
{
    const int2 cell = (int2(i.pos.xy) - int2(text.zw)) / int(text_scale.x) - 1;
    const int ch = cell.x / 4, col = cell.x % 4;
    if (cell.x >= 0 && cell.y >= 0 && cell.y < 5 && col < 3 && ch < int(text.y))
    {
        const uint glyph = kGlyphs[min((text.x >> (ch * 4)) & 0xF, 13u)];
        if ((glyph >> ((4 - cell.y) * 3 + (2 - col))) & 1)
            return float4(1, 1, 1, 1);
    }
    return float4(0, 0, 0, 1);
}
)";
        ComPtr<ID3D11GeometryShader> g_gs_rect, g_gs_quad, g_gs_point;
        ComPtr<ID3D11VertexShader> g_vs_blit;
        ComPtr<ID3D11PixelShader> g_ps_blit;
        ComPtr<ID3D11PixelShader> g_ps_depth_copy;
        ComPtr<ID3D11PixelShader> g_ps_color_copy;
        ComPtr<ID3D11PixelShader> g_ps_edram_copy;
        ComPtr<ID3D11Buffer> g_cb_edram;
        ComPtr<ID3D11Buffer> g_cb_copy;
        ComPtr<ID3D11PixelShader> g_ps_text;
        ComPtr<ID3D11Buffer> g_cb_text;
        bool g_show_fps = true; // TT2_SHOW_FPS=0 hides the counter
        uint32_t g_fps = 0;
        ComPtr<ID3D11SamplerState> g_blit_sampler;

        // ------------------------------------------------------------------ render targets

        struct Surface
        {
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            ComPtr<ID3D11ShaderResourceView> srv;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // texture format (typeless for depth)
            uint32_t width = 0, height = 0;
            // EDRAM aliasing (render targets only): where the surface starts in EDRAM, how wide its
            // tiles are, when it was last written, and how far down it has been used.
            uint32_t base = 0;
            bool depth = false;
            uint32_t tile_width = 80;   // samples per EDRAM tile row: 80 at 32 bpp, 40 at 64 bpp
            uint64_t written = 0;       // g_edram_stamp of the last draw, clear or sync into it
            uint32_t used_rows = 0;     // sample rows ever drawn
        };
        std::map<uint64_t, Surface> g_render_targets;
        uint64_t g_edram_stamp = 0;

        DXGI_FORMAT color_rt_format(uint32_t f)
        {
            switch (f)
            {
            case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;      // k_8_8_8_8
            case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;      // k_8_8_8_8_GAMMA (gamma applied by the shader writes)
            case 2: return DXGI_FORMAT_R10G10B10A2_UNORM;   // k_2_10_10_10
            case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;  // k_2_10_10_10_AS_10_10_10_10
            case 4: return DXGI_FORMAT_R16G16_FLOAT;        // k_16_16 (fixed -32..32)
            case 6: return DXGI_FORMAT_R16G16_FLOAT;        // k_16_16_FLOAT
            case 14: return DXGI_FORMAT_R32_FLOAT;          // k_32_FLOAT
            case 15: return DXGI_FORMAT_R32G32_FLOAT;       // k_32_32_FLOAT
            default: return DXGI_FORMAT_R16G16B16A16_FLOAT; // 16_16_16_16(_FLOAT), 2_10_10_10_FLOAT
            }
        }

        // Multisampled surfaces are kept at sample resolution: a pixel of a 2x surface is 1x2
        // samples, of a 4x one 2x2. Surfaces are keyed by their width in samples, so a 4x clear at
        // half the pitch (a common trick: a quarter of the pixels cover the same EDRAM) lands on the
        // same surface as the normal render target there.
        uint32_t msaa_x() { const uint32_t m = (r(RB_SURFACE_INFO) >> 16) & 3; return m >= 2 ? 2 : 1; }
        uint32_t msaa_y() { const uint32_t m = (r(RB_SURFACE_INFO) >> 16) & 3; return m >= 1 ? 2 : 1; }
        uint32_t sample_pitch() { return (r(RB_SURFACE_INFO) & 0x3FFF) * msaa_x(); }

        // 20e4 float (24-bit depth: 4-bit exponent, 20-bit mantissa) to float.
        float float20e4(uint32_t v)
        {
            const uint32_t e = (v >> 20) & 0xF, m = v & 0xFFFFF;
            if (!e)
                return std::ldexp(float(m) / 1048576.0f, -14);
            return std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15);
        }

        Surface* render_target(uint32_t info, uint32_t pitch, bool depth)
        {
            const uint32_t base = info & 0xFFF;
            const uint32_t format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
            pitch = std::clamp(pitch, 16u, 8192u);
            const uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 16) | (uint64_t(depth) << 40);
            auto it = g_render_targets.find(key);
            if (it != g_render_targets.end())
                return &it->second;
            Surface s;
            s.width = pitch;
            s.height = kRtHeight;
            s.base = base;
            s.depth = depth;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = s.width;
            d.Height = s.height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            if (depth)
            {
                s.format = format ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
                d.Format = s.format;
                d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            }
            else
            {
                s.format = color_rt_format(format);
                d.Format = s.format;
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                const bool wide = s.format == DXGI_FORMAT_R16G16B16A16_FLOAT || s.format == DXGI_FORMAT_R16G16B16A16_UNORM ||
                                  s.format == DXGI_FORMAT_R16G16B16A16_SNORM || s.format == DXGI_FORMAT_R32G32_FLOAT;
                s.tile_width = wide ? 40 : 80;
            }
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &s.tex)))
                return nullptr;
            if (depth)
            {
                D3D11_DEPTH_STENCIL_VIEW_DESC v{};
                v.Format = format ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
                v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                g_device->CreateDepthStencilView(s.tex.Get(), &v, &s.dsv);
                g_ctx->ClearDepthStencilView(s.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }
            else
            {
                g_device->CreateRenderTargetView(s.tex.Get(), nullptr, &s.rtv);
                const float zero[4] = {};
                g_ctx->ClearRenderTargetView(s.rtv.Get(), zero);
            }
            return &g_render_targets.emplace(key, std::move(s)).first->second;
        }

        // Resolved copies, by destination address.
        struct Resolved
        {
            Surface surface;
            uint32_t width = 0, height = 0;
            bool depth = false;
            uint64_t frame = 0;
            uint64_t serial = 0; // bumped by every resolve into it
        };
        std::map<uint32_t, Resolved> g_resolved;

        // A resolved copy fetched as a texture of a different size (the game renders its 3D scene
        // at a varying width, 960 or 1024, and samples the copy as a 1280-wide texture with the
        // coordinates scaled to match): the copy placed at the top left of a texture of the
        // fetched size, so texel (x, y) is the same in both, as in guest memory.
        struct FetchView
        {
            Surface surface;
            uint64_t serial = ~0ull;
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, FetchView> g_fetch_views;
        uint64_t g_frame = 0;

        // TT2_GPU_DUMP_FRAME=N: log every draw, texture and resolve of frame N and write the
        // HLSL of its shaders to native\gpu\dump.
        uint64_t g_dump_frame = ~0ull;
        fs::path g_dump_dir;
        // TT2_GPU_TRACE_BASE=hex (with the dump output): dump every draw into that EDRAM base and
        // every resolve from it, in any frame (up to 600 of them).
        bool g_trace_draw = false;
        const uint32_t g_trace_base = [] { const char* v = std::getenv("TT2_GPU_TRACE_BASE"); return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0xFFFFFFFFu; }();
        int g_traced = 0;
        bool trace_matches(uint32_t info)
        {
            static const uint64_t from = [] { const char* v = std::getenv("TT2_GPU_TRACE_FROM"); return v ? std::strtoull(v, nullptr, 10) : 0ull; }();
            const bool hit = (info & 0xFFF) == g_trace_base && g_traced < 600 && g_frame >= from;
            if (hit)
                g_traced++;
            return hit;
        }
        bool dumping() { return g_frame + 1 == g_dump_frame || g_trace_draw; }
        std::set<const void*> g_dumped_shaders;

        DXGI_FORMAT depth_srv_format(DXGI_FORMAT typeless)
        {
            return typeless == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        }

        // ------------------------------------------------------------------ textures

        struct FormatInfo
        {
            DXGI_FORMAT format;
            uint32_t block;      // pixels per block side (1 or 4)
            uint32_t bytes;      // per block
            // Xenos component -> sampled channel (4 zero, 5 one). Formats with fewer components
            // repeat their last one in the missing ones (before the fetch swizzle), as the
            // hardware does: k_8 reads as XXXX, k_8_8 as XYYY (Xenia's xenos.h).
            uint8_t map[4];
        };

        bool texture_format(uint32_t f, uint32_t sign, FormatInfo& out)
        {
            const bool snorm = sign == 1;
            const bool gamma = sign == 3;
            switch (f)
            {
            case 2: case 8: case 9: out = { snorm ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_UNORM, 1, 1, { 0, 0, 0, 0 } }; return true;
            case 3: out = { DXGI_FORMAT_B5G5R5A1_UNORM, 1, 2, { 2, 1, 0, 3 } }; return true;
            case 4: out = { DXGI_FORMAT_B5G6R5_UNORM, 1, 2, { 2, 1, 0, 0 } }; return true;
            case 6: case 14:
                out = { gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : snorm ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4, { 0, 1, 2, 3 } };
                return true;
            case 7: out = { DXGI_FORMAT_R10G10B10A2_UNORM, 1, 4, { 0, 1, 2, 3 } }; return true;
            case 10: out = { snorm ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM, 1, 2, { 0, 1, 1, 1 } }; return true;
            case 15: out = { DXGI_FORMAT_B4G4R4A4_UNORM, 1, 2, { 2, 1, 0, 3 } }; return true;
            case 18: out = { gamma ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM, 4, 8, { 0, 1, 2, 3 } }; return true;
            case 19: out = { gamma ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM, 4, 16, { 0, 1, 2, 3 } }; return true;
            case 20: out = { gamma ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM, 4, 16, { 0, 1, 2, 3 } }; return true;
            case 49: out = { DXGI_FORMAT_BC5_UNORM, 4, 16, { 0, 1, 1, 1 } }; return true;
            case 24: out = { snorm ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_UNORM, 1, 2, { 0, 0, 0, 0 } }; return true;
            case 25: out = { snorm ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM, 1, 4, { 0, 1, 1, 1 } }; return true;
            case 26: out = { snorm ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM, 1, 8, { 0, 1, 2, 3 } }; return true;
            case 30: out = { DXGI_FORMAT_R16_FLOAT, 1, 2, { 0, 0, 0, 0 } }; return true;
            case 31: out = { DXGI_FORMAT_R16G16_FLOAT, 1, 4, { 0, 1, 1, 1 } }; return true;
            case 32: out = { DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8, { 0, 1, 2, 3 } }; return true;
            case 36: out = { DXGI_FORMAT_R32_FLOAT, 1, 4, { 0, 0, 0, 0 } }; return true;
            case 37: out = { DXGI_FORMAT_R32G32_FLOAT, 1, 8, { 0, 1, 1, 1 } }; return true;
            case 38: out = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, { 0, 1, 2, 3 } }; return true;
            default: return false;
            }
        }

        int32_t tiled_2d(int32_t x, int32_t y, uint32_t pitch_aligned, uint32_t bpb_log2)
        {
            // Xenia texture_address::Tiled2D
            const int32_t outer = ((y >> 5) * int32_t(pitch_aligned >> 5) + (x >> 5)) << 6;
            const int32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
            const int32_t oi = (outer | inner) << bpb_log2;
            const uint32_t bank = (y >> 4) & 1;
            const uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return int32_t((uint32_t(y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
                   (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
        }

        struct Texture
        {
            Surface surface;
            uint64_t content = 0;
            uint64_t checked_frame = ~0ull;
            uint8_t map[4];
        };
        std::unordered_map<uint64_t, Texture> g_textures;

        struct TextureBinding
        {
            ID3D11ShaderResourceView* srv = nullptr;
            uint32_t swizzle = 0; // combined fetch swizzle and channel map
        };

        uint32_t combine_swizzle(uint32_t fetch_swizzle, const uint8_t map[4])
        {
            uint32_t out = 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                const uint32_t s = (fetch_swizzle >> (i * 3)) & 7;
                const uint32_t c = s < 4 ? map[s] : (s == 4 ? 4 : 5);
                out |= c << (i * 3);
            }
            return out;
        }

        TextureBinding texture(uint32_t slot, uint32_t dimension)
        {
            uint32_t f[6];
            for (int i = 0; i < 6; i++)
                f[i] = r(kFetchConsts + slot * 6 + i);
            TextureBinding b;
            if ((f[0] & 3) != 2)
                return b;
            const uint32_t base = f[1] & 0x1FFFF000;
            const uint32_t format = f[1] & 0x3F;
            const uint32_t endian = (f[1] >> 6) & 3;
            const bool tiled = f[0] >> 31;
            const uint32_t pitch = ((f[0] >> 22) & 0x1FF) << 5;
            const uint32_t width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
            const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
            const uint32_t sign = (f[0] >> 2) & 3;
            const bool packed_mips = (f[5] >> 11) & 1;

            if (dimension != 1 && dimension != 3)
            {
                warn_once("texture dimension " + std::to_string(dimension) + " not supported yet");
                return b;
            }
            const bool cube = dimension == 3;

            // Render-to-texture: the resolved copy of this address.
            if (auto it = g_resolved.find(base); !cube && it != g_resolved.end())
            {
                Resolved& res = it->second;
                if (!res.surface.srv)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = res.surface.format; // depth copies are R32_FLOAT
                    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    v.Texture2D.MipLevels = 1;
                    g_device->CreateShaderResourceView(res.surface.tex.Get(), &v, &res.surface.srv);
                }
                // Channels the copy has, the last one repeated (as for loaded textures).
                static const uint8_t four[4] = { 0, 1, 2, 3 }, two[4] = { 0, 1, 1, 1 }, one[4] = { 0, 0, 0, 0 };
                const DXGI_FORMAT rf = res.surface.format;
                const uint8_t* map = four;
                if (res.depth || rf == DXGI_FORMAT_R32_FLOAT)
                    map = one;
                else if (rf == DXGI_FORMAT_R16G16_FLOAT || rf == DXGI_FORMAT_R32G32_FLOAT)
                    map = two;
                b.srv = res.surface.srv.Get();
                if ((width != res.width || height != res.height) && res.surface.tex)
                {
                    FetchView& fv = g_fetch_views[{ base, width, height }];
                    if (!fv.surface.tex || fv.surface.format != res.surface.format)
                    {
                        D3D11_TEXTURE2D_DESC d{};
                        d.Width = width;
                        d.Height = height;
                        d.MipLevels = d.ArraySize = 1;
                        d.Format = res.surface.format;
                        d.SampleDesc.Count = 1;
                        d.Usage = D3D11_USAGE_DEFAULT;
                        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        fv = FetchView{};
                        fv.surface.format = res.surface.format;
                        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &fv.surface.tex)))
                        {
                            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                            v.Format = res.surface.format;
                            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            v.Texture2D.MipLevels = 1;
                            g_device->CreateShaderResourceView(fv.surface.tex.Get(), &v, &fv.surface.srv);
                        }
                    }
                    if (fv.surface.tex && fv.serial != res.serial)
                    {
                        fv.serial = res.serial;
                        const D3D11_BOX box{ 0, 0, 0, std::min(width, res.width), std::min(height, res.height), 1 };
                        g_ctx->CopySubresourceRegion(fv.surface.tex.Get(), 0, 0, 0, 0, res.surface.tex.Get(), 0, &box);
                    }
                    if (fv.surface.srv)
                        b.srv = fv.surface.srv.Get();
                }
                b.swizzle = combine_swizzle(swizzle, map);
                return b;
            }

            FormatInfo fi;
            if (!texture_format(format, sign, fi))
            {
                warn_once("texture format " + std::to_string(format) + " not supported yet");
                return b;
            }
            // Small textures with packed mips keep their base level in the packed mip tile:
            // 16 pixels down (wider than tall) or across (otherwise) in the 32x32 tile (Xenia's
            // GetPackedMipOffset).
            uint32_t ox = 0, oy = 0;
            if (packed_mips)
            {
                auto log2_ceil = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
                const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
                if (std::min(lw, lh) <= 4)
                {
                    if (lw > lh)
                        oy = 16 / fi.block;
                    else
                        ox = 16 / fi.block;
                }
            }

            const uint32_t bw = (width + fi.block - 1) / fi.block, bh = (height + fi.block - 1) / fi.block;
            const uint32_t pitch_blocks = std::max((pitch + fi.block - 1) / fi.block, bw);
            uint32_t bpb_log2 = 0;
            while ((1u << bpb_log2) < fi.bytes)
                bpb_log2++;
            const uint32_t pitch_aligned = (pitch_blocks + 31) & ~31u;
            const uint32_t height_aligned = (bh + 31) & ~31u;
            const uint32_t row_bytes_linear = (pitch_blocks * fi.bytes + 255) & ~255u;
            const uint32_t span = tiled ? pitch_aligned * height_aligned * fi.bytes : row_bytes_linear * (bh + oy);
            // Cube maps: six faces, each a 2D image at page-aligned strides.
            const uint32_t faces = cube ? 6 : 1;
            const uint32_t face_stride = (span + 4095) & ~4095u;
            const uint8_t* src = phys_ptr(base);

            uint64_t key = fnv(f, sizeof(f), cube ? 3 : 1);
            Texture& t = g_textures[key];
            if (t.checked_frame != g_frame)
            {
                t.checked_frame = g_frame;
                const uint64_t content = fnv(src, size_t(face_stride) * (faces - 1) + span);
                if (!t.surface.tex || content != t.content)
                {
                    t.content = content;
                    const size_t face_bytes = size_t(bw) * bh * fi.bytes;
                    std::vector<uint8_t> linear(face_bytes * faces);
                    for (uint32_t face = 0; face < faces; face++)
                    for (uint32_t y = 0; y < bh; y++)
                        for (uint32_t x = 0; x < bw; x++)
                        {
                            const uint32_t off = tiled ? uint32_t(tiled_2d(int32_t(x + ox), int32_t(y + oy), pitch_aligned, bpb_log2))
                                                       : (y + oy) * row_bytes_linear + (x + ox) * fi.bytes;
                            uint8_t* dst = &linear[face * face_bytes + (size_t(y) * bw + x) * fi.bytes];
                            if (off + fi.bytes > span)
                                continue;
                            std::memcpy(dst, src + size_t(face) * face_stride + off, fi.bytes);
                            if (fi.bytes >= 4)
                                for (uint32_t i = 0; i < fi.bytes; i += 4)
                                {
                                    uint32_t v;
                                    std::memcpy(&v, dst + i, 4);
                                    v = gpu_swap(v, endian);
                                    std::memcpy(dst + i, &v, 4);
                                }
                            else if (fi.bytes == 2 && (endian == 1 || endian == 2))
                                std::swap(dst[0], dst[1]);
                        }
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = width;
                    d.Height = height;
                    if (fi.block == 4)
                    {
                        d.Width = (width + 3) & ~3u;
                        d.Height = (height + 3) & ~3u;
                    }
                    d.MipLevels = 1;
                    d.ArraySize = faces;
                    d.Format = fi.format;
                    d.SampleDesc.Count = 1;
                    d.Usage = D3D11_USAGE_DEFAULT;
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
                    D3D11_SUBRESOURCE_DATA init[6];
                    for (uint32_t face = 0; face < faces; face++)
                        init[face] = { linear.data() + face * face_bytes, bw * fi.bytes, 0 };
                    t.surface = Surface{};
                    if (SUCCEEDED(g_device->CreateTexture2D(&d, init, &t.surface.tex)))
                        g_device->CreateShaderResourceView(t.surface.tex.Get(), nullptr, &t.surface.srv);
                    std::memcpy(t.map, fi.map, 4);
                }
            }
            b.srv = t.surface.srv.Get();
            b.swizzle = combine_swizzle(swizzle, t.map);
            return b;
        }

        std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> g_samplers;

        ID3D11SamplerState* sampler(uint32_t slot)
        {
            const uint32_t d0 = r(kFetchConsts + slot * 6 + 0), d3 = r(kFetchConsts + slot * 6 + 3), d5 = r(kFetchConsts + slot * 6 + 5);
            const uint64_t key = uint64_t((d0 >> 10) & 0x1FF) | (uint64_t((d3 >> 19) & 0x7FF) << 9) | (uint64_t(d5 & 3) << 20);
            auto& s = g_samplers[key];
            if (s)
                return s.Get();
            static const D3D11_TEXTURE_ADDRESS_MODE modes[8] = {
                D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
                D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
            };
            D3D11_SAMPLER_DESC d{};
            d.AddressU = modes[(d0 >> 10) & 7];
            d.AddressV = modes[(d0 >> 13) & 7];
            d.AddressW = modes[(d0 >> 16) & 7];
            const bool mag = ((d3 >> 19) & 3) == 1, min = ((d3 >> 21) & 3) == 1, mip = ((d3 >> 23) & 3) == 1;
            const uint32_t aniso = (d3 >> 25) & 7;
            if (aniso && aniso != 7)
            {
                d.Filter = D3D11_FILTER_ANISOTROPIC;
                d.MaxAnisotropy = 1u << std::min(aniso - 1, 4u);
            }
            else
                d.Filter = D3D11_FILTER((min ? 0x10 : 0) | (mag ? 0x4 : 0) | (mip ? 0x1 : 0));
            d.MaxLOD = D3D11_FLOAT32_MAX;
            const uint32_t border = d5 & 3;
            const float bc = border == 2 ? 1.0f : 0.0f;
            d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = bc;
            d.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
            d.ComparisonFunc = D3D11_COMPARISON_NEVER;
            g_device->CreateSamplerState(&d, &s);
            return s.Get();
        }

        void save_srv(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, const std::string& name, uint32_t region_w = 0, uint32_t region_h = 0);
        uint32_t g_dump_draw = 0;

        // TT2_GPU_PICK=x,y (with TT2_GPU_DUMP_FRAME): log every draw of the dumped frame that
        // changes that pixel of its first render target, with the pixel's raw value.
        int32_t g_pick_x = -1, g_pick_y = -1;

        void pick(ID3D11Resource* res)
        {
            if (g_pick_x < 0)
                return;
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))))
                return;
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            if (uint32_t(g_pick_x) >= d.Width || uint32_t(g_pick_y) >= d.Height)
                return;
            D3D11_TEXTURE2D_DESC s{};
            s.Width = s.Height = 1;
            s.MipLevels = s.ArraySize = 1;
            s.Format = d.Format;
            s.SampleDesc.Count = 1;
            s.Usage = D3D11_USAGE_STAGING;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_device->CreateTexture2D(&s, nullptr, &staging)))
                return;
            const D3D11_BOX box{ UINT(g_pick_x), UINT(g_pick_y), 0, UINT(g_pick_x + 1), UINT(g_pick_y + 1), 1 };
            g_ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return;
            uint64_t v = 0;
            std::memcpy(&v, m.pData, 8);
            g_ctx->Unmap(staging.Get(), 0);
            static std::map<ID3D11Resource*, uint64_t> last;
            auto [it, fresh] = last.try_emplace(res, ~v);
            if (it->second != v)
            {
                it->second = v;
                log("dump:   pick draw %u: target %p format %u value %016llX", g_dump_draw, (void*)res, unsigned(d.Format), (unsigned long long)v);
            }
        }
        std::set<ID3D11ShaderResourceView*> g_dumped_textures;

        // ------------------------------------------------------------------ fixed function state

        std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> g_blend_states;
        std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> g_depth_states;
        std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> g_raster_states;

        D3D11_BLEND blend_factor(uint32_t f, bool alpha)
        {
            switch (f)
            {
            case 0: return D3D11_BLEND_ZERO;
            case 1: return D3D11_BLEND_ONE;
            case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
            case 6: return D3D11_BLEND_SRC_ALPHA;
            case 7: return D3D11_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
            case 10: return D3D11_BLEND_DEST_ALPHA;
            case 11: return D3D11_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
            case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
            default: return D3D11_BLEND_ONE;
            }
        }
        D3D11_BLEND_OP blend_op(uint32_t o)
        {
            static const D3D11_BLEND_OP ops[] = { D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX,
                                                  D3D11_BLEND_OP_REV_SUBTRACT };
            return o < 5 ? ops[o] : D3D11_BLEND_OP_ADD;
        }

        ID3D11BlendState* blend_state(const uint32_t control[4], uint32_t color_mask)
        {
            uint64_t key = fnv(control, 16, color_mask);
            auto& s = g_blend_states[key];
            if (s)
                return s.Get();
            D3D11_BLEND_DESC d{};
            d.IndependentBlendEnable = TRUE;
            for (int i = 0; i < 4; i++)
            {
                const uint32_t c = control[i];
                auto& t = d.RenderTarget[i];
                t.SrcBlend = blend_factor(c & 0x1F, false);
                t.BlendOp = blend_op((c >> 5) & 7);
                t.DestBlend = blend_factor((c >> 8) & 0x1F, false);
                t.SrcBlendAlpha = blend_factor((c >> 16) & 0x1F, true);
                t.BlendOpAlpha = blend_op((c >> 21) & 7);
                t.DestBlendAlpha = blend_factor((c >> 24) & 0x1F, true);
                const bool opaque = (c & 0x1FFF) == 0x0001 && ((c >> 16) & 0x1FFF) == 0x0001;
                t.BlendEnable = !opaque;
                t.RenderTargetWriteMask = UINT8((color_mask >> (i * 4)) & 0xF);
            }
            g_device->CreateBlendState(&d, &s);
            return s.Get();
        }

        D3D11_STENCIL_OP stencil_op(uint32_t o)
        {
            static const D3D11_STENCIL_OP ops[] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR,
                                                    D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT };
            return ops[o & 7];
        }

        ID3D11DepthStencilState* depth_state(uint32_t control, uint32_t refmask)
        {
            const uint64_t key = control | (uint64_t(refmask & 0xFFFF00) << 32);
            auto& s = g_depth_states[key];
            if (s)
                return s.Get();
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = (control >> 1) & 1;
            d.DepthWriteMask = ((control >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = D3D11_COMPARISON_FUNC(((control >> 4) & 7) + 1);
            d.StencilEnable = control & 1;
            d.StencilReadMask = UINT8((refmask >> 8) & 0xFF);
            d.StencilWriteMask = UINT8((refmask >> 16) & 0xFF);
            d.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(((control >> 8) & 7) + 1);
            d.FrontFace.StencilFailOp = stencil_op(control >> 11);
            d.FrontFace.StencilPassOp = stencil_op(control >> 14);
            d.FrontFace.StencilDepthFailOp = stencil_op(control >> 17);
            if ((control >> 7) & 1)
            {
                d.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(((control >> 20) & 7) + 1);
                d.BackFace.StencilFailOp = stencil_op(control >> 23);
                d.BackFace.StencilPassOp = stencil_op(control >> 26);
                d.BackFace.StencilDepthFailOp = stencil_op(control >> 29);
            }
            else
                d.BackFace = d.FrontFace;
            g_device->CreateDepthStencilState(&d, &s);
            return s.Get();
        }

        ID3D11RasterizerState* raster_state(uint32_t mode)
        {
            const uint64_t key = mode & 0x7;
            auto& s = g_raster_states[key];
            if (s)
                return s.Get();
            D3D11_RASTERIZER_DESC d{};
            d.FillMode = D3D11_FILL_SOLID;
            const bool cf = mode & 1, cb = (mode >> 1) & 1;
            d.CullMode = (cf && !cb) ? D3D11_CULL_FRONT : (cb && !cf) ? D3D11_CULL_BACK : D3D11_CULL_NONE;
            d.FrontCounterClockwise = ((mode >> 2) & 1) == 0;
            d.DepthClipEnable = TRUE;
            d.ScissorEnable = TRUE;
            g_device->CreateRasterizerState(&d, &s);
            return s.Get();
        }

        // ------------------------------------------------------------------ draws

        void scissor(int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1)
        {
            const uint32_t tl = r(PA_SC_WINDOW_SCISSOR_TL), br = r(PA_SC_WINDOW_SCISSOR_BR), off = r(PA_SC_WINDOW_OFFSET);
            x0 = tl & 0x3FFF;
            y0 = (tl >> 16) & 0x3FFF;
            x1 = br & 0x3FFF;
            y1 = (br >> 16) & 0x3FFF;
            if (!(tl >> 31))
            {
                const int32_t ox = int32_t(off << 17) >> 17, oy = int32_t((off >> 16) << 17) >> 17;
                x0 += ox;
                x1 += ox;
                y0 += oy;
                y1 += oy;
            }
            x0 = std::max(x0, 0);
            y0 = std::max(y0, 0);
            x1 = std::max(x1, x0);
            y1 = std::max(y1, y0);
        }

        // Resolves normally stay on the host: the game samples them as textures, which finds the
        // host copy. A few it reads back with the CPU instead: the costume textures are composited
        // on the GPU (over several frames), read through the 0xC0000000 view of physical memory
        // and compressed to DXT by the CPU. Reading back every resolve would stall the GPU every
        // frame, so the copy is lazy: after a resolve, the destination's pages in the 0xC0000000
        // view are made inaccessible, and the first CPU access there (a fault) copies the latest
        // resolved picture into guest memory. Post-processing targets are never touched by the CPU
        // and never cost anything.
        constexpr uint32_t kCachedView = 0xC0000000;
        std::recursive_mutex g_lock; // the device context: GPU thread vs. a faulting CPU thread

        struct PendingWriteBack
        {
            Resolved* res;
            int32_t x0, y0, x1, y1;
            uint32_t bytes, pitch, endian, format;
            bool swap;
        };
        std::map<uint32_t, PendingWriteBack> g_pending; // by destination (physical)
        std::mutex g_pending_mutex;

        void write_back(uint32_t dest, const PendingWriteBack& w)
        {
            Resolved& res = *w.res;
            const DXGI_FORMAT f = res.surface.format;
            if ((w.format != 6 && w.format != 0) || (f != DXGI_FORMAT_R8G8B8A8_UNORM && f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) || w.x1 <= w.x0 ||
                w.y1 <= w.y0 || !res.surface.tex)
            {
                warn_once("resolve write-back of format " + std::to_string(w.format) + " not supported yet");
                return;
            }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = UINT(w.x1 - w.x0);
            d.Height = UINT(w.y1 - w.y0);
            d.MipLevels = d.ArraySize = 1;
            d.Format = f;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &staging)))
                return;
            const D3D11_BOX box{ UINT(w.x0), UINT(w.y0), 0, UINT(w.x1), UINT(w.y1), 1 };
            g_ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, res.surface.tex.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return;
            // A tiled 2D surface in the destination format, as the GPU would have written it.
            uint8_t* view = g_base + (kCachedView | dest);
            for (int32_t y = w.y0; y < w.y1; y++)
            {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y - w.y0) * m.RowPitch;
                for (int32_t x = w.x0; x < w.x1; x++)
                {
                    const uint8_t* px = row + size_t(x - w.x0) * 4;
                    // k_8_8_8_8: X in the low byte; the swap flag exchanges X and Z.
                    const uint32_t rr = w.swap ? px[2] : px[0], bb = w.swap ? px[0] : px[2];
                    const uint32_t v = gpu_swap(rr | uint32_t(px[1]) << 8 | bb << 16 | uint32_t(px[3]) << 24, w.endian);
                    std::memcpy(view + uint32_t(tiled_2d(x, y, w.pitch, 2)), &v, 4);
                }
            }
            g_ctx->Unmap(staging.Get(), 0);
            static int logged = 0;
            if (logged++ < 20)
                log("renderer: resolve %08X (%dx%d) copied to guest memory on CPU access", dest, w.x1 - w.x0, w.y1 - w.y0);
        }

        // After a colour resolve: arm the lazy copy for its destination.
        void arm_write_back(Resolved& res, uint32_t dest, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
        {
            const uint32_t info = r(RB_COPY_DEST_INFO);
            PendingWriteBack w{ &res, x0, y0, x1, y1, 0, 0, info & 7, (info >> 7) & 0x3F, ((info >> 24) & 1) != 0 };
            if (w.format != 6 && w.format != 0)
                return;
            w.pitch = std::max(((r(RB_COPY_DEST_PITCH) & 0x3FFF) + 31) & ~31u, (res.width + 31) & ~31u);
            w.bytes = (w.pitch * ((uint32_t(y1) + 31) & ~31u) * 4 + 0xFFF) & ~0xFFFu;
            if (dest + w.bytes > 0x20000000)
                return;
            std::lock_guard lock(g_pending_mutex);
            g_pending[dest] = w;
            DWORD old;
            VirtualProtect(g_base + (kCachedView | dest), w.bytes, PAGE_NOACCESS, &old);
        }

        LONG CALLBACK write_back_fault(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2 || !g_base)
                return EXCEPTION_CONTINUE_SEARCH;
            const uintptr_t addr = info->ExceptionRecord->ExceptionInformation[1];
            const uintptr_t lo = uintptr_t(g_base) + kCachedView, hi = lo + 0x20000000;
            if (addr < lo || addr >= hi)
                return EXCEPTION_CONTINUE_SEARCH;
            const uint32_t phys = uint32_t(addr - lo);
            std::lock_guard gpu(g_lock);
            std::lock_guard lock(g_pending_mutex);
            auto it = g_pending.upper_bound(phys);
            if (it == g_pending.begin())
                return EXCEPTION_CONTINUE_SEARCH;
            --it;
            if (phys >= it->first + it->second.bytes)
                return EXCEPTION_CONTINUE_SEARCH;
            const uint32_t dest = it->first;
            const PendingWriteBack w = it->second;
            g_pending.erase(it);
            DWORD old;
            VirtualProtect(g_base + (kCachedView | dest), w.bytes, PAGE_READWRITE, &old);
            write_back(dest, w);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // The console has one EDRAM; surfaces at different bases and pitches overlap in it, and games
        // rely on that (TTT2 composites costumes in a 1024x1024 surface and copies parts of it out
        // through a 512x512 surface that lies inside it). Each surface is a separate texture here,
        // so before a surface is drawn into or resolved, the parts of it that another surface wrote
        // more recently are copied in, tile by tile.
        void edram_mark(Surface& s, uint32_t rows)
        {
            s.written = ++g_edram_stamp;
            s.used_rows = std::min(std::max(s.used_rows, rows), s.height);
        }

        void edram_sync(Surface& s)
        {
            if (s.depth || !s.rtv || !g_ps_edram_copy)
                return;
            const uint32_t s_tpr = std::max(1u, s.width / s.tile_width);
            std::vector<Surface*> newer;
            for (auto& [key, o] : g_render_targets)
                if (&o != &s && !o.depth && o.written > s.written && o.tile_width == s.tile_width && o.used_rows && o.tex)
                    newer.push_back(&o);
            if (newer.empty())
                return;
            std::sort(newer.begin(), newer.end(), [](Surface* a, Surface* b) { return a->written < b->written; });
            for (Surface* o : newer)
            {
                const uint32_t o_tpr = std::max(1u, o->width / o->tile_width);
                const uint32_t o_end = std::min(2048u, o->base + (o->used_rows + 15) / 16 * o_tpr);
                const uint32_t s_end = std::min(2048u, s.base + (s.height / 16) * s_tpr);
                const uint32_t lo = std::max(s.base, o->base), hi = std::min(s_end, o_end);
                if (lo >= hi)
                    continue;
                if (!o->srv)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = o->format;
                    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    v.Texture2D.MipLevels = 1;
                    g_device->CreateShaderResourceView(o->tex.Get(), &v, &o->srv);
                }
                if (!o->srv)
                    continue;
                const uint32_t row0 = (lo - s.base) / s_tpr * 16, row1 = std::min(s.height, (hi - s.base + s_tpr - 1) / s_tpr * 16);
                const uint32_t consts[8] = { s.base, s_tpr, o->base, o_tpr, s.tile_width, o->used_rows, 0, 0 };
                update_cbuffer(g_cb_edram.Get(), consts, sizeof(consts));
                ID3D11ShaderResourceView* none[32] = {};
                g_ctx->PSSetShaderResources(0, 32, none);
                g_ctx->OMSetRenderTargets(1, s.rtv.GetAddressOf(), nullptr);
                const D3D11_VIEWPORT vp{ 0, 0, float(s.width), float(s.height), 0, 1 };
                const D3D11_RECT sc{ 0, LONG(row0), LONG(s.width), LONG(row1) };
                g_ctx->RSSetViewports(1, &vp);
                g_ctx->RSSetScissorRects(1, &sc);
                g_ctx->RSSetState(raster_state(0));
                g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                g_ctx->OMSetDepthStencilState(nullptr, 0);
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_ctx->IASetInputLayout(nullptr);
                g_ctx->VSSetShader(g_vs_blit.Get(), nullptr, 0);
                g_ctx->GSSetShader(nullptr, nullptr, 0);
                g_ctx->PSSetShader(g_ps_edram_copy.Get(), nullptr, 0);
                g_ctx->PSSetConstantBuffers(5, 1, g_cb_edram.GetAddressOf());
                g_ctx->PSSetShaderResources(0, 1, o->srv.GetAddressOf());
                g_ctx->Draw(3, 0);
                g_ctx->PSSetShaderResources(0, 1, none);
                g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
                s.used_rows = std::max(s.used_rows, row1);
                static int logged = 0;
                if (logged++ < 20)
                    log("renderer: EDRAM alias: surface %03X (pitch %u) takes rows %u-%u from surface %03X (pitch %u)", s.base, s.width, row0, row1,
                        o->base, o->width);
            }
            s.written = ++g_edram_stamp;
        }

        void resolve()
        {
            const uint32_t control = r(RB_COPY_CONTROL);
            const uint32_t command = (control >> 20) & 3;
            const uint32_t src_select = control & 7;
            const bool depth = src_select >= 4;
            g_trace_draw = g_trace_base != 0xFFFFFFFFu && !depth && trace_matches(r(RB_COLOR_INFO_REGS[src_select & 3]));
            const uint32_t pitch = sample_pitch();
            const uint32_t sx = msaa_x(), sy = msaa_y();
            Surface* src = render_target(depth ? r(RB_DEPTH_INFO) : r(RB_COLOR_INFO_REGS[src_select & 3]), pitch, depth);
            if (!src)
                return;
            if (!depth)
                edram_sync(*src);

            // The rectangle: the resolve "draw" is a rectangle list of 3 float2 vertices in vf0.
            const uint32_t vf0 = r(kFetchConsts), vf1 = r(kFetchConsts + 1);
            int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            {
                const uint8_t* p = phys_ptr(vf0 & ~3u);
                float v[6];
                for (int i = 0; i < 6; i++)
                {
                    uint32_t u;
                    std::memcpy(&u, p + i * 4, 4);
                    u = gpu_swap(u, vf1 & 3);
                    std::memcpy(&v[i], &u, 4);
                }
                const float half = (r(PA_SU_VTX_CNTL) & 1) ? 0.0f : 0.5f;
                auto fix = [&](float f) { return int32_t(std::floor(f + half + 0.5f)); };
                x0 = std::min({ fix(v[0]), fix(v[2]), fix(v[4]) });
                x1 = std::max({ fix(v[0]), fix(v[2]), fix(v[4]) });
                y0 = std::min({ fix(v[1]), fix(v[3]), fix(v[5]) });
                y1 = std::max({ fix(v[1]), fix(v[3]), fix(v[5]) });
                if ((r(PA_SU_SC_MODE_CNTL) >> 16) & 1)
                {
                    const uint32_t off = r(PA_SC_WINDOW_OFFSET);
                    const int32_t ox = int32_t(off << 17) >> 17, oy = int32_t((off >> 16) << 17) >> 17;
                    x0 += ox;
                    x1 += ox;
                    y0 += oy;
                    y1 += oy;
                }
                int32_t sx0, sy0, sx1, sy1;
                scissor(sx0, sy0, sx1, sy1);
                x0 = std::clamp(x0, sx0, sx1);
                x1 = std::clamp(x1, sx0, sx1);
                y0 = std::clamp(y0, sy0, sy1);
                y1 = std::clamp(y1, sy0, sy1);
                x1 = std::min<int32_t>(x1, int32_t(src->width / sx));
                y1 = std::min<int32_t>(y1, int32_t(src->height / sy));
            }

            if (command != 3 && x1 > x0 && y1 > y0)
            {
                const uint32_t dest = r(RB_COPY_DEST_BASE) & 0x1FFFF000;
                const uint32_t dp = r(RB_COPY_DEST_PITCH);
                const uint32_t dw = std::max(dp & 0x3FFF, uint32_t(x1)), dh = std::max((dp >> 16) & 0x3FFF, uint32_t(y1));
                Resolved& res = g_resolved[dest];
                // Depth is copied by a shader pass into an R32_FLOAT texture: Direct3D 11 only copies
                // depth-stencil resources whole, between textures of the same size. Multisampled
                // colour is copied by a pass too (sample 0 of each pixel).
                const DXGI_FORMAT dest_format = depth ? DXGI_FORMAT_R32_FLOAT : src->format;
                const bool by_pass = depth || sx > 1 || sy > 1;
                if (!res.surface.tex || res.width != dw || res.height != dh || res.surface.format != dest_format)
                {
                    const uint64_t serial = res.serial; // fetch views compare against it
                    res = Resolved{};
                    res.serial = serial;
                    res.width = dw;
                    res.height = dh;
                    res.depth = depth;
                    res.surface.format = dest_format;
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = dw;
                    d.Height = dh;
                    d.MipLevels = 1;
                    d.ArraySize = 1;
                    d.Format = dest_format;
                    d.SampleDesc.Count = 1;
                    d.Usage = D3D11_USAGE_DEFAULT;
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    g_device->CreateTexture2D(&d, nullptr, &res.surface.tex);
                    if (res.surface.tex)
                        g_device->CreateRenderTargetView(res.surface.tex.Get(), nullptr, &res.surface.rtv);
                }
                res.frame = g_frame;
                res.serial++;
                // TT2_GPU_DUMP_ON_RESOLVE=<dest hex>: dump the frame after a resolve there (the Nth burst of
                // them with TT2_GPU_DUMP_NTH, TT2_GPU_DUMP_DELAY frames later).
                static const uint32_t dump_on = [] { const char* v = std::getenv("TT2_GPU_DUMP_ON_RESOLVE"); return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u; }();
                static const int dump_nth = [] { const char* v = std::getenv("TT2_GPU_DUMP_NTH"); return v ? std::atoi(v) : 1; }();
                static int dump_bursts = 0;
                static uint64_t dump_last = ~0ull;
                if (dump_on && dest == dump_on && (dump_last == ~0ull || g_frame > dump_last + 120))
                    dump_bursts++;
                if (dump_on && dest == dump_on)
                    dump_last = g_frame;
                static bool dump_armed = true;
                if (dump_on && dest == dump_on && dump_armed && dump_bursts == dump_nth)
                {
                    dump_armed = false;
                    const char* delay = std::getenv("TT2_GPU_DUMP_DELAY");
                    g_dump_frame = g_frame + 2 + (delay ? std::strtoull(delay, nullptr, 10) : 0);
                    log("renderer: resolve to %08X at frame %llu, dumping the next frame", dest, (unsigned long long)g_frame);
                }
                if (dumping())
                    log("dump: resolve %s %d,%d-%d,%d src info %08X pitch %u -> %08X (%ux%u) control %08X dest info %08X", depth ? "depth" : "color", x0, y0, x1,
                        y1, depth ? r(RB_DEPTH_INFO) : r(RB_COLOR_INFO_REGS[src_select & 3]), pitch, dest, dw, dh, control, r(RB_COPY_DEST_INFO));
                static int logged = 0;
                static std::set<uint32_t> seen_dests;
                if (logged++ < 12 || seen_dests.insert(dest).second)
                    log("renderer: resolve %s %d,%d-%d,%d to %08X (%ux%u, format %u)", depth ? "depth" : "color", x0, y0, x1, y1, dest, dw, dh,
                        (r(RB_COPY_DEST_INFO) >> 7) & 0x3F);
                if (res.surface.tex)
                {
                    D3D11_BOX box{ UINT(x0), UINT(y0), 0, UINT(std::min<int32_t>(x1, int32_t(dw))), UINT(std::min<int32_t>(y1, int32_t(dh))), 1 };
                    if (box.right > box.left && box.bottom > box.top)
                    {
                        if (!by_pass)
                            g_ctx->CopySubresourceRegion(res.surface.tex.Get(), 0, UINT(x0), UINT(y0), 0, src->tex.Get(), 0, &box);
                        else if (res.surface.rtv)
                        {
                            if (!src->srv)
                            {
                                D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                                v.Format = depth ? depth_srv_format(src->format) : src->format;
                                v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                v.Texture2D.MipLevels = 1;
                                g_device->CreateShaderResourceView(src->tex.Get(), &v, &src->srv);
                            }
                            const uint32_t scale[4] = { sx, sy, 0, 0 };
                            update_cbuffer(g_cb_copy.Get(), scale, sizeof(scale));
                            g_ctx->PSSetConstantBuffers(3, 1, g_cb_copy.GetAddressOf());
                            g_ctx->OMSetRenderTargets(1, res.surface.rtv.GetAddressOf(), nullptr); // also unbinds the depth surface
                            const D3D11_VIEWPORT vp{ 0, 0, float(dw), float(dh), 0, 1 };
                            const D3D11_RECT sc{ LONG(box.left), LONG(box.top), LONG(box.right), LONG(box.bottom) };
                            g_ctx->RSSetViewports(1, &vp);
                            g_ctx->RSSetScissorRects(1, &sc);
                            g_ctx->RSSetState(raster_state(0));
                            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                            g_ctx->OMSetDepthStencilState(nullptr, 0);
                            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                            g_ctx->IASetInputLayout(nullptr);
                            g_ctx->VSSetShader(g_vs_blit.Get(), nullptr, 0);
                            g_ctx->GSSetShader(nullptr, nullptr, 0);
                            g_ctx->PSSetShader(depth ? g_ps_depth_copy.Get() : g_ps_color_copy.Get(), nullptr, 0);
                            g_ctx->PSSetShaderResources(0, 1, src->srv.GetAddressOf());
                            g_ctx->Draw(3, 0);
                            ID3D11ShaderResourceView* none = nullptr;
                            g_ctx->PSSetShaderResources(0, 1, &none);
                            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
                        }
                    }
                    if (!depth)
                        arm_write_back(res, dest, x0, y0, std::min<int32_t>(x1, int32_t(dw)), std::min<int32_t>(y1, int32_t(dh)));
                    if (dumping() && depth)
                    {
                        // Actual depth values of the copy (an 8-bit preview cannot show them).
                        D3D11_TEXTURE2D_DESC d;
                        res.surface.tex->GetDesc(&d);
                        d.Usage = D3D11_USAGE_STAGING;
                        d.BindFlags = 0;
                        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> st;
                        D3D11_MAPPED_SUBRESOURCE m;
                        if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &st)))
                        {
                            g_ctx->CopyResource(st.Get(), res.surface.tex.Get());
                            if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)))
                            {
                                const bool f32 = true; // R32_FLOAT
                                double lo = 1e30, hi = -1e30, sum = 0;
                                uint32_t n = 0;
                                for (uint32_t y = 0; y < d.Height; y += 8)
                                    for (uint32_t x = 0; x < d.Width; x += 8)
                                    {
                                        const uint8_t* px = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x * 4;
                                        uint32_t u;
                                        std::memcpy(&u, px, 4);
                                        float v;
                                        if (f32)
                                            std::memcpy(&v, &u, 4);
                                        else
                                            v = float(u & 0xFFFFFF) / 16777215.0f;
                                        lo = std::min<double>(lo, v);
                                        hi = std::max<double>(hi, v);
                                        sum += v;
                                        n++;
                                    }
                                g_ctx->Unmap(st.Get(), 0);
                                log("dump:   depth values of %08X: min %g max %g mean %g", dest, lo, hi, n ? sum / n : 0.0);
                            }
                        }
                    }
                }
            }

            // Clears that come with the resolve.
            if ((control >> 8) & 1)
            {
                Surface* c = render_target(r(RB_COLOR_INFO_REGS[src_select & 3]), pitch, false);
                if (c && c->rtv)
                {
                    const uint32_t v = r(RB_COLOR_CLEAR);
                    const float color[4] = { ((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, (v >> 24) / 255.0f };
                    const D3D11_RECT rect{ LONG(x0 * sx), LONG(y0 * sy), LONG(x1 * sx), LONG(y1 * sy) };
                    if (g_ctx1 && x1 > x0 && y1 > y0)
                        g_ctx1->ClearView(c->rtv.Get(), color, &rect, 1);
                    else
                        g_ctx->ClearRenderTargetView(c->rtv.Get(), color);
                    edram_mark(*c, uint32_t(std::max(0, y1)) * sy);
                }
            }
            if ((control >> 9) & 1)
            {
                Surface* d = render_target(r(RB_DEPTH_INFO), pitch, true);
                if (d && d->dsv)
                {
                    const uint32_t v = r(RB_DEPTH_CLEAR);
                    const float z = ((r(RB_DEPTH_INFO) >> 16) & 1) ? float20e4(v >> 8) : float(v >> 8) / 16777215.0f;
                    g_ctx->ClearDepthStencilView(d->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, UINT8(v & 0xFF));
                }
            }
        }

        void fill_swapped16(void* dst, const void* src, uint32_t bytes)
        {
            const uint16_t* s = static_cast<const uint16_t*>(src);
            uint16_t* d = static_cast<uint16_t*>(dst);
            for (uint32_t i = 0; i < bytes / 2; i++)
                d[i] = __builtin_bswap16(s[i]);
        }
        void fill_swapped32(void* dst, const void* src, uint32_t bytes)
        {
            const uint32_t* s = static_cast<const uint32_t*>(src);
            uint32_t* d = static_cast<uint32_t*>(dst);
            for (uint32_t i = 0; i < bytes / 4; i++)
                d[i] = __builtin_bswap32(s[i]);
        }

        void do_draw(uint32_t initiator, uint32_t index_base, uint32_t index_size)
        {
            const uint32_t prim = initiator & 0x3F;
            const uint32_t source = (initiator >> 6) & 3;
            const bool index32 = (initiator >> 11) & 1;
            const uint32_t count = initiator >> 16;
            if (!count || source == 1)
            {
                if (source == 1)
                    warn_once("immediate index data not supported yet");
                return;
            }
            const uint32_t edram_mode = r(RB_MODECONTROL) & 7;
            if (edram_mode == 6)
            {
                resolve();
                return;
            }
            if (edram_mode != 4 && edram_mode != 5)
            {
                static uint32_t n = 0;
                if (n++ < 20)
                    log("renderer: draw with edram mode %u (prim %u count %u, vs memexport %d)", edram_mode, prim, count,
                        g_current_vs && prepare(*g_current_vs) ? int(g_current_vs->meta.memexport) : -1);
                return;
            }
            if (!g_current_vs || !g_current_ps || !prepare(*g_current_vs))
                return;
            const bool depth_only = edram_mode == 5;
            if (!depth_only && !prepare(*g_current_ps))
                return;
            const TranslatedShader& vm = g_current_vs->meta;
            const TranslatedShader& pm = g_current_ps->meta;
            g_trace_draw = g_trace_base != 0xFFFFFFFFu && !depth_only && trace_matches(r(RB_COLOR_INFO));
            if (g_trace_draw)
                log("dump: traced draw in frame %llu", (unsigned long long)g_frame);
            if (dumping())
            {
                g_dump_draw++;
                log("dump: draw %u: prim %u count %u source %u mode %u | vs %p ps %p | surface %08X color %08X/%08X depth %08X dc %08X mask %04X blend %08X vte %08X scissor %08X-%08X "
                    "| vp scale %g %g off %g %g",
                    g_dump_draw, prim, count, source, edram_mode, (void*)g_current_vs, (void*)g_current_ps, r(RB_SURFACE_INFO), r(RB_COLOR_INFO), r(0x2003),
                    r(RB_DEPTH_INFO), r(RB_DEPTHCONTROL), r(RB_COLOR_MASK) & 0xFFFF, r(RB_BLENDCONTROL0), r(PA_CL_VTE_CNTL), r(PA_SC_WINDOW_SCISSOR_TL),
                    r(PA_SC_WINDOW_SCISSOR_BR), rf(PA_CL_VPORT_XSCALE), rf(PA_CL_VPORT_XSCALE + 2), rf(PA_CL_VPORT_XSCALE + 1), rf(PA_CL_VPORT_XSCALE + 3));
                {
                    const uint32_t vbase = r(SQ_VS_CONST) & 0x1FF, pbase = r(SQ_PS_CONST) & 0x1FF;
                    auto cf = [&](uint32_t i) { return rf(kAluConsts + std::min(vbase * 4 + i, 2047u)); };
                    log("dump:   vs const base %u ps base %u | c128 %g %g %g %g | c129 %g %g %g %g | c130 %g %g %g %g | c131 %g %g %g %g | c132 %g %g", vbase, pbase,
                        cf(512), cf(513), cf(514), cf(515), cf(516), cf(517), cf(518), cf(519), cf(520), cf(521), cf(522), cf(523), cf(524), cf(525), cf(526),
                        cf(527), cf(528), cf(529));
                    {
                        // Pixel shader constants c0-c15 and c248-c255.
                        std::string ps;
                        char buf[96];
                        for (uint32_t i : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 248u, 249u, 250u, 251u, 252u, 253u, 254u, 255u })
                        {
                            const uint32_t at = std::min((256u + i) * 4u, 2044u);
                            snprintf(buf, sizeof(buf), " c%u %g %g %g %g |", i, rf(kAluConsts + at), rf(kAluConsts + at + 1), rf(kAluConsts + at + 2), rf(kAluConsts + at + 3));
                            ps += buf;
                        }
                        log("dump:   ps consts%s", ps.c_str());
                    }
                    for (uint32_t slot : vm.vertex_slots)
                    {
                        const uint32_t d0 = r(kFetchConsts + slot * 2), d1 = r(kFetchConsts + slot * 2 + 1);
                        const uint8_t* vp = phys_ptr(d0 & ~3u);
                        float v[8];
                        for (int i = 0; i < 8; i++)
                        {
                            uint32_t u;
                            std::memcpy(&u, vp + i * 4, 4);
                            u = gpu_swap(u, d1 & 3);
                            std::memcpy(&v[i], &u, 4);
                        }
                        log("dump:   vf%u addr %08X size %u endian %u | %g %g %g %g %g %g %g %g", slot, d0 & ~3u, (d1 >> 2) & 0xFFFFFF, d1 & 3, v[0], v[1], v[2], v[3],
                            v[4], v[5], v[6], v[7]);
                    }
                }
                for (Shader* sh : { g_current_vs, g_current_ps })
                    if (sh && g_dumped_shaders.insert(sh).second && !sh->meta.hlsl.empty())
                    {
                        std::error_code ec;
                        fs::create_directories(g_dump_dir, ec);
                        char name[64];
                        snprintf(name, sizeof(name), "%s_%p.hlsl", sh->stage == ShaderStage::Vertex ? "vs" : "ps", (void*)sh);
                        std::ofstream(g_dump_dir / name) << sh->meta.hlsl;
                    }
            }

            // ---- render targets
            const uint32_t pitch = sample_pitch();
            const uint32_t sx = msaa_x(), sy = msaa_y();
            const uint32_t color_mask = r(RB_COLOR_MASK);
            ID3D11RenderTargetView* rtvs[4] = {};
            uint32_t rt_width = 0, rt_height = kRtHeight;
            for (uint32_t i = 0; i < 4 && !depth_only; i++)
                if ((pm.color_targets & (1u << i)) && ((color_mask >> (i * 4)) & 0xF))
                    if (Surface* s = render_target(r(RB_COLOR_INFO_REGS[i]), pitch, false))
                    {
                        edram_sync(*s);
                        int32_t sx0_, sy0_, sx1_, sy1_;
                        scissor(sx0_, sy0_, sx1_, sy1_);
                        edram_mark(*s, uint32_t(std::max(0, sy1_)) * sy);
                        rtvs[i] = s->rtv.Get();
                        rt_width = s->width;
                    }
            const uint32_t depth_control = r(RB_DEPTHCONTROL);
            ID3D11DepthStencilView* dsv = nullptr;
            if (depth_control & 3)
                if (Surface* s = render_target(r(RB_DEPTH_INFO), pitch, true))
                {
                    dsv = s->dsv.Get();
                    rt_width = s->width;
                }
            if (!rt_width)
                return;
            // Unbind textures that might be the targets, then bind.
            ID3D11ShaderResourceView* null_srvs[32] = {};
            g_ctx->PSSetShaderResources(0, 32, null_srvs);
            g_ctx->OMSetRenderTargets(4, rtvs, dsv);

            // ---- viewport: the guest's viewport transform goes into the vertex shader
            DrawConsts dc{};
            const uint32_t vte = r(PA_CL_VTE_CNTL);
            float xs = (vte & 1) ? rf(PA_CL_VPORT_XSCALE) : 1.0f, xo = (vte & 2) ? rf(PA_CL_VPORT_XSCALE + 1) : 0.0f;
            float ys = (vte & 4) ? rf(PA_CL_VPORT_XSCALE + 2) : 1.0f, yo = (vte & 8) ? rf(PA_CL_VPORT_XSCALE + 3) : 0.0f;
            float zs = (vte & 16) ? rf(PA_CL_VPORT_XSCALE + 4) : 1.0f, zo = (vte & 32) ? rf(PA_CL_VPORT_XSCALE + 5) : 0.0f;
            if ((r(PA_SU_SC_MODE_CNTL) >> 16) & 1)
            {
                const uint32_t off = r(PA_SC_WINDOW_OFFSET);
                xo += float(int32_t(off << 17) >> 17);
                yo += float(int32_t((off >> 16) << 17) >> 17);
            }
            if (!(r(PA_SU_VTX_CNTL) & 1)) // Direct3D 9 pixel centers
            {
                xo += 0.5f;
                yo += 0.5f;
            }
            xs *= float(sx);
            xo *= float(sx);
            ys *= float(sy);
            yo *= float(sy);
            const float W = float(rt_width), H = float(rt_height);
            dc.ndc_scale[0] = xs * 2.0f / W;
            dc.ndc_offset[0] = xo * 2.0f / W - 1.0f;
            dc.ndc_scale[1] = -ys * 2.0f / H;
            dc.ndc_offset[1] = 1.0f - yo * 2.0f / H;
            dc.ndc_scale[2] = zs;
            dc.ndc_offset[2] = zo;
            // vtx_w0_fmt clear means the shader writes 1/W (Xenia: kSysFlag_WNotReciprocal).
            dc.draw_flags[0] = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | ((((vte >> 10) & 1) ^ 1) << 2);
            const uint32_t program = r(SQ_PROGRAM_CNTL);
            if ((program >> 18) & 1)
                dc.draw_flags[1] = ((r(SQ_CONTEXT_MISC) >> 8) & 0xFF) + 1;
            const uint32_t colorcontrol = r(RB_COLORCONTROL);
            dc.draw_flags[2] = (colorcontrol & 7) | (((colorcontrol >> 3) & 1) << 3);
            dc.alpha_ref[0] = rf(RB_ALPHA_REF);
            const uint32_t ps_size = r(PA_SU_POINT_SIZE);
            dc.point_size[0] = float(ps_size >> 16) / 16.0f * float(sx);
            dc.point_size[1] = float(ps_size & 0xFFFF) / 16.0f * float(sy);
            dc.point_size[2] = 2.0f / W;
            dc.point_size[3] = 2.0f / H;

            D3D11_VIEWPORT vp{ 0, 0, W, H, 0.0f, 1.0f };
            g_ctx->RSSetViewports(1, &vp);
            int32_t sx0, sy0, sx1, sy1;
            scissor(sx0, sy0, sx1, sy1);
            sx0 *= int32_t(sx);
            sx1 *= int32_t(sx);
            sy0 *= int32_t(sy);
            sy1 *= int32_t(sy);
            const D3D11_RECT sc{ sx0, sy0, std::min<LONG>(sx1, LONG(W)), std::min<LONG>(sy1, LONG(H)) };
            g_ctx->RSSetScissorRects(1, &sc);

            // ---- vertex data: only the vertices this draw can reach (the fetch constant's size
            // is often the whole rest of the buffer)
            uint32_t max_index = count - 1;
            if (source == 0)
            {
                const uint32_t endian = index_size >> 30;
                const bool i32 = index32;
                const uint8_t* ip = phys_ptr(index_base & ~(i32 ? 3u : 1u));
                max_index = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t v;
                    if (i32)
                    {
                        std::memcpy(&v, ip + i * 4, 4);
                        if (endian)
                            v = __builtin_bswap32(v);
                    }
                    else
                    {
                        uint16_t h;
                        std::memcpy(&h, ip + i * 2, 2);
                        v = endian ? __builtin_bswap16(h) : h;
                        if (v == 0xFFFF)
                            continue;
                    }
                    if (v != 0xFFFFFFFF)
                        max_index = std::max(max_index, v);
                }
            }
            max_index += r(VGT_INDX_OFFSET);
            for (size_t vi = 0; vi < vm.vertex_slots.size(); vi++)
            {
                const uint32_t slot = vm.vertex_slots[vi];
                const uint32_t d0 = r(kFetchConsts + slot * 2), d1 = r(kFetchConsts + slot * 2 + 1);
                const uint64_t reach = (uint64_t(max_index) * vm.vertex_strides[vi] + vm.vertex_extents[vi]) * 4;
                const uint32_t address = d0 & ~3u, bytes = uint32_t(std::min<uint64_t>(((d1 >> 2) & 0xFFFFFF) * 4ull, reach));
                const uint32_t at = bytes ? g_vertex_ring.write(phys_ptr(address), bytes) : 0;
                dc.vfetch[slot >> 1][(slot & 1) * 2] = at == ~0u ? 0 : at;
                dc.vfetch[slot >> 1][(slot & 1) * 2 + 1] = d1 & 3;
            }

            // ---- textures
            ID3D11ShaderResourceView* vs_srvs[33] = {};
            ID3D11ShaderResourceView* ps_srvs[32] = {};
            ID3D11SamplerState* vs_smps[32] = {};
            ID3D11SamplerState* ps_smps[32] = {};
            uint32_t vs_max = 0, ps_max = 0;
            auto bind_textures = [&](const TranslatedShader& m, ID3D11ShaderResourceView** srvs, ID3D11SamplerState** smps, uint32_t& max) {
                for (const ShaderTexture& t : m.textures)
                {
                    TextureBinding b = texture(t.slot, t.dimension);
                    if (dumping() && b.srv && g_dumped_textures.insert(b.srv).second)
                    {
                        const uint32_t f2 = r(kFetchConsts + t.slot * 6 + 2);
                        char name[64];
                        snprintf(name, sizeof(name), "tex_%08X.png", r(kFetchConsts + t.slot * 6 + 1) & 0x1FFFF000);
                        save_srv(b.srv, std::min((f2 & 0x1FFF) + 1, 2048u), std::min(((f2 >> 13) & 0x1FFF) + 1, 2048u), name);
                    }
                    if (dumping())
                    {
                        const uint32_t f0 = r(kFetchConsts + t.slot * 6), f1 = r(kFetchConsts + t.slot * 6 + 1), f2 = r(kFetchConsts + t.slot * 6 + 2);
                        log("dump:   tf%u dim %u base %08X format %u %ux%u tiled %u -> %s%s", t.slot, t.dimension, f1 & 0x1FFFF000, f1 & 0x3F,
                            (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f0 >> 31, b.srv ? "bound" : "MISSING",
                            g_resolved.count(f1 & 0x1FFFF000) ? " (resolved)" : "");
                        // How much of the texture's first page is non-zero (CPU-generated textures).
                        const uint8_t* raw = phys_ptr(f1 & 0x1FFFF000);
                        uint32_t nonzero = 0;
                        for (uint32_t i = 0; i < 4096; i++)
                            nonzero += raw[i] != 0;
                        log("dump:     raw %02X%02X%02X%02X %02X%02X%02X%02X, %u of 4096 bytes non-zero", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                            raw[6], raw[7], nonzero);
                    }
                    srvs[t.slot] = b.srv;
                    smps[t.slot] = sampler(t.slot);
                    dc.tex_swizzle[t.slot >> 2][t.slot & 3] = b.swizzle;
                    max = std::max(max, t.slot + 1);
                }
            };
            bind_textures(vm, vs_srvs, vs_smps, vs_max);
            if (!depth_only)
                bind_textures(pm, ps_srvs, ps_smps, ps_max);
            vs_srvs[32] = g_vertex_ring.srv.Get();

            // ---- constants
            {
                // The register file holds the values big-endian; the constant buffers want them native.
                uint32_t consts[1024];
                auto upload = [&](ID3D11Buffer* b, uint32_t base) {
                    for (uint32_t i = 0; i < 1024; i++)
                        consts[i] = r(kAluConsts + std::min(base * 4 + i, 2047u));
                    update_cbuffer(b, consts, sizeof(consts));
                };
                upload(g_cb_vs_float.Get(), r(SQ_VS_CONST) & 0x1FF);
                upload(g_cb_ps_float.Get(), r(SQ_PS_CONST) & 0x1FF);
                IntConsts ic;
                for (uint32_t i = 0; i < 8; i++)
                    ic.bools[i] = r(kBoolConsts + i);
                for (uint32_t i = 0; i < 32; i++)
                    ic.loops[i] = r(kLoopConsts + i);
                update_cbuffer(g_cb_int.Get(), &ic, sizeof(ic));
                update_cbuffer(g_cb_draw.Get(), &dc, sizeof(dc));
            }
            ID3D11Buffer* vs_cbs[3] = { g_cb_vs_float.Get(), g_cb_int.Get(), g_cb_draw.Get() };
            ID3D11Buffer* ps_cbs[3] = { g_cb_ps_float.Get(), g_cb_int.Get(), g_cb_draw.Get() };
            g_ctx->VSSetConstantBuffers(0, 3, vs_cbs);
            g_ctx->PSSetConstantBuffers(0, 3, ps_cbs);
            g_ctx->GSSetConstantBuffers(0, 3, vs_cbs);
            g_ctx->VSSetShaderResources(0, 33, vs_srvs);
            if (vs_max)
                g_ctx->VSSetSamplers(0, vs_max, vs_smps);
            g_ctx->PSSetShaderResources(0, 32, ps_srvs);
            if (ps_max)
                g_ctx->PSSetSamplers(0, ps_max, ps_smps);

            // ---- state
            uint32_t blend[4];
            for (int i = 0; i < 4; i++)
                blend[i] = r(RB_BLENDCONTROL_REGS[i]);
            const float blend_factor_v[4] = { rf(RB_BLEND_RED), rf(RB_BLEND_RED + 1), rf(RB_BLEND_RED + 2), rf(RB_BLEND_RED + 3) };
            g_ctx->OMSetBlendState(blend_state(blend, color_mask), blend_factor_v, 0xFFFFFFFF);
            const uint32_t refmask = r(RB_STENCILREFMASK);
            g_ctx->OMSetDepthStencilState(depth_state(depth_control, refmask), refmask & 0xFF);
            g_ctx->RSSetState(raster_state(r(PA_SU_SC_MODE_CNTL)));
            g_ctx->VSSetShader(g_current_vs->vs.Get(), nullptr, 0);
            g_ctx->PSSetShader(depth_only ? nullptr : g_current_ps->ps.Get(), nullptr, 0);
            g_ctx->IASetInputLayout(nullptr);

            // ---- primitive type
            ID3D11GeometryShader* gs = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY topology;
            switch (prim)
            {
            case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; gs = g_gs_point.Get(); break;
            case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
            case 3: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
            case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
            case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break; // fan: indices rebuilt below
            case 6: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
            case 8: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; gs = g_gs_rect.Get(); break;
            case 13: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ; gs = g_gs_quad.Get(); break;
            default:
                warn_once("primitive type " + std::to_string(prim) + " not supported yet");
                return;
            }
            g_ctx->GSSetShader(gs, nullptr, 0);
            g_ctx->IASetPrimitiveTopology(topology);

            const int32_t base_vertex = int32_t(r(VGT_INDX_OFFSET));
            struct AfterDraw
            {
                uint32_t prim, count;
                ID3D11RenderTargetView* rtv;
                ~AfterDraw()
                {
                    // A 256x144 thumbnail of the 1024x720 corner after every draw (points are skipped).
                    if (!dumping() || prim == 1 || !rtv)
                        return;
                    ComPtr<ID3D11Resource> res;
                    rtv->GetResource(&res);
                    pick(res.Get());
                    ComPtr<ID3D11ShaderResourceView> srv;
                    g_device->CreateShaderResourceView(res.Get(), nullptr, &srv);
                    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
                    char name[64];
                    snprintf(name, sizeof(name), "draw_%04u.png", g_dump_draw);
                    if (srv)
                        save_srv(srv.Get(), 256, 144, name, 1024, 720);
                }
            } after_draw{ prim, count, rtvs[0] };
            if (source == 0) // indexed
            {
                const uint32_t endian = index_size >> 30;
                const uint32_t isz = index32 ? 4 : 2;
                const uint8_t* src = phys_ptr(index_base & ~(isz - 1));
                std::vector<uint8_t> fan;
                const void* data = src;
                uint32_t n = count;
                auto fill = endian == 0 ? nullptr : (index32 ? (endian == 2 ? fill_swapped32 : nullptr) : fill_swapped16);
                if (index32 && endian == 3)
                    warn_once("16-in-32 index swap not supported");
                if (prim == 5)
                {
                    // Fan to list, converting endianness here.
                    std::vector<uint32_t> idx(count);
                    for (uint32_t i = 0; i < count; i++)
                        idx[i] = index32 ? (endian ? __builtin_bswap32(reinterpret_cast<const uint32_t*>(src)[i]) : reinterpret_cast<const uint32_t*>(src)[i])
                                         : (endian ? __builtin_bswap16(reinterpret_cast<const uint16_t*>(src)[i]) : reinterpret_cast<const uint16_t*>(src)[i]);
                    n = count >= 3 ? (count - 2) * 3 : 0;
                    fan.resize(n * 4);
                    uint32_t* out = reinterpret_cast<uint32_t*>(fan.data());
                    for (uint32_t i = 2; i < count; i++)
                    {
                        *out++ = idx[0];
                        *out++ = idx[i - 1];
                        *out++ = idx[i];
                    }
                    data = fan.data();
                    fill = nullptr;
                }
                const bool as32 = index32 || prim == 5;
                const uint32_t at = g_index_ring.write(data, n * (as32 ? 4 : 2), fill);
                if (at == ~0u)
                    return;
                g_ctx->IASetIndexBuffer(g_index_ring.buffer.Get(), as32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, at);
                g_ctx->DrawIndexed(n, 0, base_vertex);
            }
            else // auto-generated indices
            {
                if (prim == 5)
                {
                    std::vector<uint32_t> idx;
                    for (uint32_t i = 2; i < count; i++)
                        idx.insert(idx.end(), { 0u, i - 1, i });
                    const uint32_t at = g_index_ring.write(idx.data(), uint32_t(idx.size() * 4));
                    if (at == ~0u)
                        return;
                    g_ctx->IASetIndexBuffer(g_index_ring.buffer.Get(), DXGI_FORMAT_R32_UINT, at);
                    g_ctx->DrawIndexed(uint32_t(idx.size()), 0, base_vertex);
                }
                else
                    g_ctx->Draw(count, 0);
            }
        }

        // ------------------------------------------------------------------ presentation

        void write_png(const fs::path& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t row_pitch)
        {
            // Uncompressed (stored) deflate: big but trivial.
            std::vector<uint8_t> raw;
            raw.reserve(size_t(h) * (w * 4 + 1));
            for (uint32_t y = 0; y < h; y++)
            {
                raw.push_back(0);
                const uint8_t* row = rgba + size_t(y) * row_pitch;
                for (uint32_t x = 0; x < w; x++)
                {
                    raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
                    raw.push_back(255);
                }
            }
            auto crc = [](const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFF) {
                for (size_t i = 0; i < n; i++)
                {
                    c ^= p[i];
                    for (int k = 0; k < 8; k++)
                        c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
                }
                return c;
            };
            std::vector<uint8_t> z = { 0x78, 0x01 };
            uint32_t a = 1, b = 0;
            for (size_t i = 0; i < raw.size();)
            {
                const size_t n = std::min<size_t>(65535, raw.size() - i);
                z.push_back(i + n == raw.size() ? 1 : 0);
                z.push_back(uint8_t(n));
                z.push_back(uint8_t(n >> 8));
                z.push_back(uint8_t(~n));
                z.push_back(uint8_t(~n >> 8));
                z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
                i += n;
            }
            for (uint8_t c : raw)
            {
                a = (a + c) % 65521;
                b = (b + a) % 65521;
            }
            const uint32_t adler = (b << 16) | a;
            for (int s = 24; s >= 0; s -= 8)
                z.push_back(uint8_t(adler >> s));
            std::ofstream f(path, std::ios::binary);
            auto be32 = [&](uint32_t v) {
                const uint8_t q[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
                f.write(reinterpret_cast<const char*>(q), 4);
            };
            auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
                be32(uint32_t(data.size()));
                std::vector<uint8_t> td(type, type + 4);
                td.insert(td.end(), data.begin(), data.end());
                f.write(reinterpret_cast<const char*>(td.data()), std::streamsize(td.size()));
                be32(~crc(td.data(), td.size()));
            };
            f.write("\x89PNG\r\n\x1a\n", 8);
            std::vector<uint8_t> ihdr = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                          uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0 };
            chunk("IHDR", ihdr);
            chunk("IDAT", z);
            chunk("IEND", {});
        }

        // Draws a texture into an RGBA8 target of w x h (its top-left w x h texels, alpha ignored)
        // and writes it as a PNG to the dump directory.
        // region_w x region_h (default: w x h) texels of the top-left corner, scaled to w x h.
        void save_srv(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, const std::string& name, uint32_t region_w, uint32_t region_h)
        {
            ComPtr<ID3D11Resource> res;
            srv->GetResource(&res);
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(res.As(&tex)))
                return;
            D3D11_TEXTURE2D_DESC src;
            tex->GetDesc(&src);
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &target)) || FAILED(g_device->CreateRenderTargetView(target.Get(), nullptr, &rtv)))
                return;
            // Map the w x h corner of the source onto the target.
            const float sx = region_w ? float(w) / float(region_w) : 1.0f, sy = region_h ? float(h) / float(region_h) : 1.0f;
            D3D11_VIEWPORT vp{ 0, 0, float(src.Width) * sx, float(src.Height) * sy, 0, 1 };
            const D3D11_RECT sc{ 0, 0, LONG(w), LONG(h) };
            g_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->RSSetScissorRects(1, &sc);
            g_ctx->RSSetState(raster_state(0));
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g_ctx->OMSetDepthStencilState(nullptr, 0);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->VSSetShader(g_vs_blit.Get(), nullptr, 0);
            g_ctx->GSSetShader(nullptr, nullptr, 0);
            g_ctx->PSSetShader(g_ps_blit.Get(), nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->PSSetSamplers(0, 1, g_blit_sampler.GetAddressOf());
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &none);
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &staging)))
                return;
            g_ctx->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return;
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    const uint8_t* p = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x * 4;
                    uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                    o[0] = p[2];
                    o[1] = p[1];
                    o[2] = p[0];
                    o[3] = 255;
                }
            g_ctx->Unmap(staging.Get(), 0);
            std::error_code ec;
            fs::create_directories(g_dump_dir, ec);
            write_png(g_dump_dir / name, rgba.data(), w, h, w * 4);
        }

        void screenshot(ID3D11Texture2D* backbuffer)
        {
            D3D11_TEXTURE2D_DESC d;
            backbuffer->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &staging)))
                return;
            g_ctx->CopyResource(staging.Get(), backbuffer);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                return;
            // BGRA -> RGBA
            std::vector<uint8_t> rgba(size_t(d.Width) * d.Height * 4);
            for (uint32_t y = 0; y < d.Height; y++)
                for (uint32_t x = 0; x < d.Width; x++)
                {
                    const uint8_t* s = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x * 4;
                    uint8_t* o = &rgba[(size_t(y) * d.Width + x) * 4];
                    o[0] = s[2];
                    o[1] = s[1];
                    o[2] = s[0];
                    o[3] = 255;
                }
            g_ctx->Unmap(staging.Get(), 0);
            std::error_code ec;
            fs::create_directories(g_screenshot_dir, ec);
            // Two instances on one PC (TT2_LOG_NAME set): prefix the file with the log name.
            const char* prefix = std::getenv("TT2_LOG_NAME");
            char name[160];
            snprintf(name, sizeof(name), "%s%sframe_%06llu.png", prefix ? prefix : "", prefix ? "_" : "", (unsigned long long)g_frame);
            write_png(g_screenshot_dir / name, rgba.data(), d.Width, d.Height, d.Width * 4);
        }
    }

    bool init(void* hwnd, const fs::path& screenshot_dir)
    {
        if (const char* v = std::getenv("TT2_RENDERER"); v && std::string(v) == "null")
            return false;
        HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
        g_compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!g_compile)
        {
            log("renderer: d3dcompiler_47.dll not found");
            return false;
        }
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (const char* v = std::getenv("TT2_D3D_DEBUG"); v && *v == '1')
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx)))
        {
            log("renderer: no Direct3D 11 device");
            return false;
        }
        g_device.As(&g_device1);
        g_ctx.As(&g_ctx1);

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        g_device.As(&dxgi_device);
        dxgi_device->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_ADAPTER_DESC ad;
        adapter->GetDesc(&ad);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
        g_hwnd = static_cast<HWND>(hwnd);
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_backbuffer_w = std::max<LONG>(rc.right - rc.left, 1);
        g_backbuffer_h = std::max<LONG>(rc.bottom - rc.top, 1);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = g_backbuffer_w;
        sd.Height = g_backbuffer_h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // Tearing allowed: a presented frame goes to the screen right away instead of waiting for
        // the desktop compositor's next refresh (about a frame less input lag; the game paces
        // itself to 60 fps). TT2_VSYNC=1 turns it off.
        {
            ComPtr<IDXGIFactory5> f5;
            BOOL tearing = FALSE;
            const char* vs = std::getenv("TT2_VSYNC");
            if ((!vs || *vs != '1') && SUCCEEDED(factory.As(&f5)) &&
                SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
            {
                sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
                g_tearing = true;
            }
        }
        if (FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), g_hwnd, &sd, nullptr, nullptr, &g_swapchain)))
        {
            log("renderer: cannot create the swap chain");
            return false;
        }
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        // Queue at most one frame ahead of the GPU (the default is three, ~50 ms of input lag).
        ComPtr<IDXGIDevice1> dxgi_device1;
        if (SUCCEEDED(g_device.As(&dxgi_device1)))
            dxgi_device1->SetMaximumFrameLatency(1);

        std::string error;
        auto gs = [&](const char* entry, ComPtr<ID3D11GeometryShader>& out) {
            if (auto code = compile(kFixedHlsl, entry, "gs_5_0", &error))
                g_device->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out);
            else
                log("renderer: %s: %s", entry, error.c_str());
        };
        gs("gs_rect", g_gs_rect);
        gs("gs_quad", g_gs_quad);
        gs("gs_point", g_gs_point);
        if (auto code = compile(kFixedHlsl, "vs_blit", "vs_5_0", &error))
            g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vs_blit);
        if (auto code = compile(kFixedHlsl, "ps_text", "ps_5_0", &error))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_text);
        else
            log("renderer: ps_text: %s", error.c_str());
        if (auto code = compile(kFixedHlsl, "ps_edram_copy", "ps_5_0", &error))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_edram_copy);
        else
            log("renderer: ps_edram_copy: %s", error.c_str());
        if (auto code = compile(kFixedHlsl, "ps_color_copy", "ps_5_0", &error))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_color_copy);
        else
            log("renderer: ps_color_copy: %s", error.c_str());
        if (auto code = compile(kFixedHlsl, "ps_depth_copy", "ps_5_0", &error))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_depth_copy);
        else
            log("renderer: ps_depth_copy: %s", error.c_str());
        if (auto code = compile(kFixedHlsl, "ps_blit", "ps_5_0", &error))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_blit);
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        g_device->CreateSamplerState(&smp, &g_blit_sampler);

        g_vertex_ring.create(64u << 20, D3D11_BIND_SHADER_RESOURCE, true);
        g_index_ring.create(16u << 20, D3D11_BIND_INDEX_BUFFER, false);
        g_cb_vs_float = make_cbuffer(4096);
        g_cb_ps_float = make_cbuffer(4096);
        g_cb_int = make_cbuffer(sizeof(IntConsts));
        g_cb_draw = make_cbuffer(sizeof(DrawConsts));
        g_cb_copy = make_cbuffer(16);
        g_cb_edram = make_cbuffer(32);
        g_cb_text = make_cbuffer(32);
        if (const char* v = std::getenv("TT2_SHOW_FPS"))
            g_show_fps = std::atoi(v) != 0;

        g_screenshot_dir = screenshot_dir;
        if (const char* v = std::getenv("TT2_SCREENSHOT"); v && std::atoi(v) > 0)
            g_screenshot_interval = uint32_t(std::atoi(v));
        if (const char* v = std::getenv("TT2_GPU_DUMP_FRAME"); v && std::atoll(v) > 0)
            g_dump_frame = uint64_t(std::atoll(v));
        if (const char* v = std::getenv("TT2_GPU_PICK"); v && std::strchr(v, ','))
        {
            g_pick_x = std::atoi(v);
            g_pick_y = std::atoi(std::strchr(v, ',') + 1);
        }
        g_dump_dir = screenshot_dir.parent_path() / "gpu" / "dump";
        g_shader_cache = screenshot_dir.parent_path() / "cache" / "shaders";
        start_workers();
        AddVectoredExceptionHandler(1, write_back_fault);
        g_active = true;
        log("renderer: Direct3D 11 on %s", name);
        return true;
    }

    bool active() { return g_active; }

    void load_shader(uint32_t type, const uint32_t* words_be, uint32_t count)
    {
        if (!g_active || !count)
            return;
        const ShaderStage stage = type == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
        const uint64_t key = fnv(words_be, size_t(count) * 4, type + 1);
        auto& s = g_shaders[key];
        if (!s)
        {
            s = std::make_unique<Shader>();
            s->stage = stage;
            s->words.assign(words_be, words_be + count);
            start_compile(s.get());
        }
        (stage == ShaderStage::Vertex ? g_current_vs : g_current_ps) = s.get();
    }

    void draw(uint32_t initiator, uint32_t index_base, uint32_t index_size)
    {
        std::lock_guard lock(g_lock);
        if (g_active)
            do_draw(initiator, index_base, index_size);
    }

    namespace
    {
        // Frames presented per second, counted over half-second windows, drawn at the top right.
        void draw_fps(ID3D11RenderTargetView* rtv)
        {
            using clock = std::chrono::steady_clock;
            static clock::time_point window = clock::now();
            static uint32_t frames = 0;
            frames++;
            const double elapsed = std::chrono::duration<double>(clock::now() - window).count();
            if (elapsed >= 0.5)
            {
                g_fps = uint32_t(frames / elapsed + 0.5);
                frames = 0;
                window = clock::now();
            }
            // "<n> FPS"
            uint32_t chars[8], n = 0;
            char digits[12];
            const int len = snprintf(digits, sizeof(digits), "%u", std::min(g_fps, 9999u));
            for (int i = 0; i < len; i++)
                chars[n++] = uint32_t(digits[i] - '0');
            chars[n++] = 13;
            chars[n++] = 10;
            chars[n++] = 11;
            chars[n++] = 12;
            uint32_t packed = 0;
            for (uint32_t i = 0; i < n; i++)
                packed |= chars[i] << (i * 4);
            const uint32_t scale = std::max(2u, g_backbuffer_h / 240);
            const uint32_t bw = (4 * n + 1) * scale, bh = 7 * scale, margin = 2 * scale;
            const uint32_t x = g_backbuffer_w > bw + margin ? g_backbuffer_w - bw - margin : 0;
            const uint32_t consts[8] = { packed, n, x, margin, scale, 0, 0, 0 };
            update_cbuffer(g_cb_text.Get(), consts, sizeof(consts));
            const D3D11_VIEWPORT vp{ float(x), float(margin), float(bw), float(bh), 0.0f, 1.0f };
            const D3D11_RECT sc{ LONG(x), LONG(margin), LONG(x + bw), LONG(margin + bh) };
            g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->RSSetScissorRects(1, &sc);
            g_ctx->RSSetState(raster_state(0));
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g_ctx->OMSetDepthStencilState(nullptr, 0);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->VSSetShader(g_vs_blit.Get(), nullptr, 0);
            g_ctx->GSSetShader(nullptr, nullptr, 0);
            g_ctx->PSSetShader(g_ps_text.Get(), nullptr, 0);
            g_ctx->PSSetConstantBuffers(4, 1, g_cb_text.GetAddressOf());
            g_ctx->Draw(3, 0);
        }
    }

    void swap(uint32_t frontbuffer_physical, uint32_t width, uint32_t height)
    {
        std::lock_guard lock(g_lock);
        if (!g_active)
            return;
        g_frame++;
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        const uint32_t w = std::max<LONG>(rc.right - rc.left, 1), h = std::max<LONG>(rc.bottom - rc.top, 1);
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (w != g_backbuffer_w || h != g_backbuffer_h)
        {
            g_ctx->ClearState();
            if (SUCCEEDED(g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
            {
                g_backbuffer_w = w;
                g_backbuffer_h = h;
            }
        }
        ComPtr<ID3D11Texture2D> back;
        g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
        ComPtr<ID3D11RenderTargetView> rtv;
        g_device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        const float black[4] = { 0, 0, 0, 1 };
        g_ctx->ClearRenderTargetView(rtv.Get(), black);

        auto it = g_resolved.find(frontbuffer_physical & 0x1FFFF000);
        if (it != g_resolved.end() && it->second.surface.tex && !it->second.depth)
        {
            Resolved& res = it->second;
            if (!res.surface.srv)
                g_device->CreateShaderResourceView(res.surface.tex.Get(), nullptr, &res.surface.srv);
            // The front buffer may be larger than the picture; show width x height of it, letterboxed.
            const float fw = float(width ? width : res.width), fh = float(height ? height : res.height);
            const float scale = std::min(float(g_backbuffer_w) / fw, float(g_backbuffer_h) / fh);
            const float vw = fw * scale, vh = fh * scale;
            D3D11_VIEWPORT vp{ (g_backbuffer_w - vw) * 0.5f, (g_backbuffer_h - vh) * 0.5f, vw * float(res.width) / fw,
                               vh * float(res.height) / fh, 0.0f, 1.0f };
            const D3D11_RECT sc{ LONG(vp.TopLeftX), LONG(vp.TopLeftY), LONG(vp.TopLeftX + vw), LONG(vp.TopLeftY + vh) };
            g_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->RSSetScissorRects(1, &sc);
            g_ctx->RSSetState(raster_state(0));
            g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            g_ctx->OMSetDepthStencilState(nullptr, 0);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_ctx->IASetInputLayout(nullptr);
            g_ctx->VSSetShader(g_vs_blit.Get(), nullptr, 0);
            g_ctx->GSSetShader(nullptr, nullptr, 0);
            g_ctx->PSSetShader(g_ps_blit.Get(), nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, res.surface.srv.GetAddressOf());
            g_ctx->PSSetSamplers(0, 1, g_blit_sampler.GetAddressOf());
            g_ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            g_ctx->PSSetShaderResources(0, 1, &none);
        }
        else
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                log("renderer: front buffer %08X (%ux%u) was not resolved", frontbuffer_physical, width, height);
            }
        }
        if (g_show_fps && g_ps_text)
            draw_fps(rtv.Get());
        if (g_frame == g_dump_frame)
            log("dump: swap front buffer %08X %ux%u (%s)", frontbuffer_physical, width, height,
                g_resolved.count(frontbuffer_physical & 0x1FFFF000) ? "resolved" : "not resolved");
        if ((g_screenshot_interval && g_frame % g_screenshot_interval == 0) || g_frame == g_dump_frame)
            screenshot(back.Get());
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_swapchain->Present(0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
}
