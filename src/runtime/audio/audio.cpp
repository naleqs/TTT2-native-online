// XAudio render driver: the game's audio engine registers a callback that renders 256-sample
// frames of 6-channel float audio and hands them over with XAudioSubmitRenderDriverFrame.
// Frames are downmixed to stereo and queued on an SDL audio stream; the callback is driven
// from a runtime thread at the stream's pace.
//
// XMA (the console's hardware audio codec) is not decoded yet, so compressed music/voice
// streams are silent; XMACreateContext hands out inert contexts.

#include "audio.h"
#include "../kernel/threads.h"
#include "../memory.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

using namespace tt2;

namespace tt2::audio
{
    namespace
    {
        constexpr int kFrameSamples = 256;
        constexpr int kRate = 48000;

        SDL_AudioStream* g_stream = nullptr;
        constexpr uint32_t kMaxClients = 8;
        constexpr uint32_t kDriverBase = 0x41550000;
        struct Client
        {
            std::atomic<uint32_t> callback{ 0 }, arg{ 0 };
        };
        Client g_clients[kMaxClients];
        // The frame being mixed: every client's submission of this tick is added in.
        float g_mix[kFrameSamples * 2];
        bool g_mixed = false;
        std::atomic<bool> g_running{ false };
        std::atomic<bool> g_muted{ false };
        std::thread g_thread;

        void driver_thread()
        {
            kernel::adopt_host_thread("audio");
            const auto frame = std::chrono::microseconds(1'000'000 * kFrameSamples / kRate);
            auto next = std::chrono::steady_clock::now();
            while (g_running)
            {
                // Keep ~3 frames queued: enough to ride out jitter, little latency.
                const int queued = g_stream ? SDL_GetAudioStreamQueued(g_stream) : 0;
                if (queued < int(3 * kFrameSamples * 2 * sizeof(float)))
                {
                    std::fill(std::begin(g_mix), std::end(g_mix), 0.0f);
                    g_mixed = false;
                    for (Client& c : g_clients)
                        if (const uint32_t cb = c.callback)
                            call_guest(cb, uint32_t(c.arg));
                    if (g_mixed && g_stream && !g_muted)
                    {
                        for (float& v : g_mix)
                            v = std::clamp(v, -1.0f, 1.0f);
                        SDL_PutAudioStreamData(g_stream, g_mix, sizeof(g_mix));
                    }
                }
                next += frame;
                const auto now = std::chrono::steady_clock::now();
                if (next < now - std::chrono::milliseconds(50))
                    next = now;
                std::this_thread::sleep_until(next);
            }
        }
    }

    bool init()
    {
        SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, kRate };
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream)
        {
            log("audio: no output device (%s)", SDL_GetError());
            return false;
        }
        SDL_ResumeAudioStreamDevice(g_stream);
        return true;
    }

    void shutdown()
    {
        if (g_running.exchange(false) && g_thread.joinable())
            g_thread.join();
        if (g_stream)
        {
            SDL_DestroyAudioStream(g_stream);
            g_stream = nullptr;
        }
    }

    void set_muted(bool muted) { g_muted = muted; }

    uint32_t register_client(uint32_t callback, uint32_t arg)
    {
        for (uint32_t i = 0; i < kMaxClients; i++)
        {
            uint32_t expected = 0;
            if (g_clients[i].callback.compare_exchange_strong(expected, callback))
            {
                g_clients[i].arg = arg;
                if (!g_running.exchange(true))
                    g_thread = std::thread(driver_thread);
                return kDriverBase | i;
            }
        }
        log("audio: more than %u render driver clients", kMaxClients);
        return 0;
    }

    void unregister_client(uint32_t driver)
    {
        if ((driver & 0xFFFF0000) == kDriverBase && (driver & 0xFFFF) < kMaxClients)
            g_clients[driver & 0xFFFF].callback = 0;
    }

    void submit(uint32_t driver, uint32_t samples)
    {
        (void)driver;
        if (!samples)
            return;
        // Channel-major: FL, FR, C, LFE, SL, SR, 256 big-endian floats each.
        auto sample = [&](int channel, int i) { return std::bit_cast<float>(load32(samples + uint32_t(channel * kFrameSamples + i) * 4)); };
        for (int i = 0; i < kFrameSamples; i++)
        {
            const float c = sample(2, i) * 0.7071f;
            const float l = sample(0, i) + c + sample(4, i) * 0.7071f;
            const float r = sample(1, i) + c + sample(5, i) * 0.7071f;
            g_mix[i * 2] += l;
            g_mix[i * 2 + 1] += r;
        }
        g_mixed = true;
    }
}

// XAudioRegisterRenderDriverClient(callback_info*, driver*)  callback_info: be32 fn, be32 arg
TT2_IMPORT(XAudioRegisterRenderDriverClient)
{
    Args a{ ctx };
    const uint32_t info = a.u32(0);
    // The callback receives a pointer to a dword holding the client's argument, not the
    // argument itself (as Xenia's AudioSystem::RegisterClient does).
    const uint32_t wrapped = g_memory.system_alloc(4);
    store32(wrapped, load32(info + 4));
    const uint32_t driver = audio::register_client(load32(info), wrapped);
    if (!driver)
    {
        a.ret(0x80004005); // E_FAIL
        return;
    }
    if (auto* d = a.ptr<be<uint32_t>>(1))
        *d = driver;
    a.ret(0);
}

TT2_IMPORT(XAudioUnregisterRenderDriverClient)
{
    audio::unregister_client(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XAudioSubmitRenderDriverFrame)
{
    audio::submit(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XAudioGetSpeakerConfig)
{
    if (ctx.r3.u32)
        store32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32)
        store32(ctx.r4.u32, std::bit_cast<uint32_t>(1.0f));
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XAudioEnableDucker) { ctx.r3.u64 = 0; }
TT2_IMPORT(XAudioGetDuckerLevel) { if (ctx.r3.u32) store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
TT2_IMPORT(XAudioGetDuckerThreshold) { if (ctx.r3.u32) store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
TT2_IMPORT(XAudioGetDuckerAttackTime) { if (ctx.r3.u32) store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
TT2_IMPORT(XAudioGetDuckerReleaseTime) { if (ctx.r3.u32) store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }
TT2_IMPORT(XAudioGetDuckerHoldTime) { if (ctx.r3.u32) store32(ctx.r3.u32, 0); ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------- XMA

// Stand-in for the console's XMA decoder hardware. XAudio2 hands compressed buffers to XMA
// contexts and waits until the decoder consumes them and fills the output ring; without that
// progress every voice stays queued forever (the game then blocks waiting for its sounds to
// end). This decoder produces silence at a plausible pace. Context layout (16 big-endian
// dwords of bitfields) as documented by Xenia's xma_context.h. A real decoder (FFmpeg's xma2,
// as Xenia uses) can replace fill_silence() later.
//
// XAudio2 drives the decoder through its registers at 0x7FEA0000 (Xenia's xma_decoder.cc):
// contexts are numbered by their index in the array at ContextArrayAddress, and the Kick, Lock
// and Clear register groups (one bit per context) enable, disable and reset them. The
// registers are little-endian. There is no MMIO trap here, so the XMA thread polls the
// command registers and clears them as it applies them.
namespace
{
    std::mutex g_xma_mutex;
    std::vector<uint32_t> g_xma_contexts; // allocated, by guest address

    constexpr uint32_t kXmaRegisters = 0x7FEA0000;
    constexpr uint32_t kXmaContextCount = 320;
    constexpr uint32_t kXmaContextArrayAddress = 0x600, kXmaCurrentContext = 0x606;
    constexpr uint32_t kXmaKick = 0x650, kXmaLock = 0x690, kXmaClear = 0x6A0;
    uint32_t g_xma_array = 0; // guest address of the context array
    std::vector<bool> g_xma_used, g_xma_enabled;
    uint32_t g_xma_rotate = 0;

    uint32_t* xma_register(uint32_t index) { return reinterpret_cast<uint32_t*>(g_base + kXmaRegisters + index * 4); }
    uint32_t xma_context(uint32_t id) { return g_xma_array + id * 64; }

    // Applies the commands XAudio2 wrote since the last pass. Called with g_xma_mutex held.
    void xma_commands()
    {
        for (uint32_t group = 0; group < 10; group++)
            for (uint32_t command : { kXmaKick, kXmaLock, kXmaClear })
            {
                uint32_t bits = __atomic_exchange_n(xma_register(command + group), 0u, __ATOMIC_ACQ_REL);
                for (uint32_t i = 0; bits; i++, bits >>= 1)
                {
                    const uint32_t id = group * 32 + i;
                    if (!(bits & 1) || id >= kXmaContextCount)
                        continue;
                    static int logged = 0;
                    if (logged++ < 40)
                        log("audio: XMA %s context %u", command == kXmaKick ? "kick" : command == kXmaLock ? "lock" : "clear", id);
                    if (command == kXmaKick)
                        g_xma_enabled[id] = true;
                    else if (command == kXmaLock)
                        g_xma_enabled[id] = false;
                    else
                    {
                        // Reset: both input buffers and the output buffer invalid, offsets 0.
                        const uint32_t c = xma_context(id);
                        store32(c + 0, load32(c + 0) & ~((1u << 20) | (1u << 21) | (0x1Fu << 27)));
                        store32(c + 4, load32(c + 4) & ~(1u << 31));
                        store32(c + 36, load32(c + 36) & ~0x1Fu);
                    }
                }
            }
        // The context the hardware is "processing" rotates, so no context looks stuck.
        *xma_register(kXmaCurrentContext) = g_xma_rotate = (g_xma_rotate + 1) % kXmaContextCount;
    }
    std::atomic<bool> g_xma_running{ false };
    std::thread g_xma_thread;

    uint32_t phys_to_guest(uint32_t p) { return p >= 0x20000000 ? p : (p | 0xA0000000); }

    // When a voice is stopped, nobody reads its output ring. Once a ring has been full and
    // unread for a while, input keeps being consumed (and the silent output dropped) so that
    // XAudio2 can release the voice's buffers, e.g. FlushSourceBuffers when a movie stops.
    struct XmaStall
    {
        uint32_t read = ~0u;
        std::chrono::steady_clock::time_point since;
    };
    std::unordered_map<uint32_t, XmaStall> g_xma_stalls;
    constexpr auto kXmaStallTimeout = std::chrono::milliseconds(50);

    void xma_work(uint32_t c)
    {
        uint32_t d0 = load32(c + 0), d1 = load32(c + 4), d2 = load32(c + 8);
        const uint32_t d9 = load32(c + 36);
        const bool output_valid = (d1 >> 31) & 1;
        const bool valid0 = (d0 >> 20) & 1, valid1 = (d0 >> 21) & 1;
        if (!output_valid || (!valid0 && !valid1))
            return;

        const uint32_t blocks = (d0 >> 22) & 0x1F;
        uint32_t write = (d0 >> 27) & 0x1F;
        const uint32_t read = d9 & 0x1F;
        const bool stereo = (d1 >> 29) & 1;
        const uint32_t frame_blocks = stereo ? 8 : 4; // 512 samples x 16 bit x channels / 256
        if (!blocks || frame_blocks > blocks)
            return;

        const uint32_t output = phys_to_guest(load32(c + 28));
        uint32_t current = (load32(c + 16) >> 31) & 1;
        uint32_t read_bits = d2 & 0x3FFFFFF;

        const auto now = std::chrono::steady_clock::now();
        XmaStall& stall = g_xma_stalls[c];
        if (stall.read != read)
        {
            stall.read = read;
            stall.since = now;
        }
        const bool stalled = now - stall.since > kXmaStallTimeout;

        // Decode (silently) while there is room for a whole frame in the output ring.
        for (int frames = 0; frames < 8; frames++)
        {
            const uint32_t free_blocks = (read + blocks - write - 1) % blocks;
            const bool room = free_blocks >= frame_blocks;
            if (!room && !stalled)
                break;
            const bool cur_valid = current ? ((d0 >> 21) & 1) : ((d0 >> 20) & 1);
            if (!cur_valid)
            {
                const bool other_valid = current ? ((d0 >> 20) & 1) : ((d0 >> 21) & 1);
                if (!other_valid)
                    break;
                current ^= 1;
                read_bits = 32; // past the first packet header
            }
            if (room)
            {
                for (uint32_t b = 0; b < frame_blocks; b++)
                    std::memset(g_base + output + ((write + b) % blocks) * 256, 0, 256);
                write = (write + frame_blocks) % blocks;
            }

            // About one 2 KB packet per four frames of output.
            const uint32_t packets = current ? (d1 & 0xFFF) : (d0 & 0xFFF);
            read_bits += 2048 * 8 / 4;
            if (read_bits >= packets * 2048 * 8)
            {
                d0 &= ~(1u << (20 + current)); // input buffer consumed
                read_bits = 32;
                current ^= 1;
            }
        }

        d0 = (d0 & ~(0x1Fu << 27)) | (write << 27);
        d2 = (d2 & ~0x3FFFFFFu) | (read_bits & 0x3FFFFFF);
        store32(c + 0, d0);
        store32(c + 8, d2);
        store32(c + 16, (load32(c + 16) & 0x7FFFFFFF) | (current << 31));
    }

    void xma_thread()
    {
        while (g_xma_running)
        {
            {
                std::lock_guard lock(g_xma_mutex);
                xma_commands();
                for (uint32_t c : g_xma_contexts)
                    if (g_xma_enabled[(c - g_xma_array) / 64])
                        xma_work(c);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void audio::init_xma()
{
    g_xma_array = g_memory.physical.alloc(kXmaContextCount * 64, 0x1000);
    std::memset(g_base + g_xma_array, 0, kXmaContextCount * 64);
    g_xma_used.assign(kXmaContextCount, false);
    g_xma_enabled.assign(kXmaContextCount, false);
    *xma_register(kXmaContextArrayAddress) = g_xma_array & 0x1FFFFFFF; // physical
    *xma_register(kXmaCurrentContext) = 0;
}

TT2_IMPORT(XMACreateContext)
{
    uint32_t context = 0;
    {
        std::lock_guard lock(g_xma_mutex);
        for (uint32_t id = 0; id < kXmaContextCount; id++)
            if (!g_xma_used[id])
            {
                g_xma_used[id] = true;
                g_xma_enabled[id] = false;
                context = xma_context(id);
                std::memset(g_base + context, 0, 64);
                g_xma_contexts.push_back(context);
                log("audio: XMA context %u created", id);
                break;
            }
    }
    if (!context)
    {
        log("audio: out of XMA contexts");
        ctx.r3.u64 = 0xC0000017; // STATUS_NO_MEMORY
        return;
    }
    if (!g_xma_running.exchange(true))
        g_xma_thread = std::thread(xma_thread);
    if (ctx.r3.u32)
        store32(ctx.r3.u32, context);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XMAReleaseContext)
{
    const uint32_t context = ctx.r3.u32;
    {
        std::lock_guard lock(g_xma_mutex);
        std::erase(g_xma_contexts, context);
        g_xma_stalls.erase(context);
        if (context >= g_xma_array && context < g_xma_array + kXmaContextCount * 64)
        {
            const uint32_t id = (context - g_xma_array) / 64;
            g_xma_used[id] = false;
            g_xma_enabled[id] = false;
            std::memset(g_base + context, 0, 64);
        }
    }
    ctx.r3.u64 = 0;
}
