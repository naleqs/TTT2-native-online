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
#include <thread>

using namespace tt2;

namespace tt2::audio
{
    namespace
    {
        constexpr int kFrameSamples = 256;
        constexpr int kRate = 48000;

        SDL_AudioStream* g_stream = nullptr;
        std::atomic<uint32_t> g_callback{ 0 }, g_callback_arg{ 0 };
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
                if (g_callback && queued < int(3 * kFrameSamples * 2 * sizeof(float)))
                    call_guest(g_callback, uint32_t(g_callback_arg));
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

    void register_client(uint32_t callback, uint32_t arg)
    {
        g_callback = callback;
        g_callback_arg = arg;
        if (!g_running.exchange(true))
            g_thread = std::thread(driver_thread);
    }

    void unregister_client()
    {
        g_callback = 0;
    }

    void submit(uint32_t samples)
    {
        if (!g_stream || g_muted || !samples)
            return;
        // Channel-major: FL, FR, C, LFE, SL, SR, 256 big-endian floats each.
        float stereo[kFrameSamples * 2];
        auto sample = [&](int channel, int i) { return std::bit_cast<float>(load32(samples + uint32_t(channel * kFrameSamples + i) * 4)); };
        for (int i = 0; i < kFrameSamples; i++)
        {
            const float c = sample(2, i) * 0.7071f;
            const float l = sample(0, i) + c + sample(4, i) * 0.7071f;
            const float r = sample(1, i) + c + sample(5, i) * 0.7071f;
            stereo[i * 2] = std::clamp(l, -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(r, -1.0f, 1.0f);
        }
        SDL_PutAudioStreamData(g_stream, stereo, sizeof(stereo));
    }
}

// XAudioRegisterRenderDriverClient(callback_info*, driver*)  callback_info: be32 fn, be32 arg
TT2_IMPORT(XAudioRegisterRenderDriverClient)
{
    Args a{ ctx };
    const uint32_t info = a.u32(0);
    audio::register_client(load32(info), load32(info + 4));
    if (auto* d = a.ptr<be<uint32_t>>(1))
        *d = 0x41550000;
    a.ret(0);
}

TT2_IMPORT(XAudioUnregisterRenderDriverClient)
{
    audio::unregister_client();
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XAudioSubmitRenderDriverFrame)
{
    audio::submit(ctx.r4.u32);
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

TT2_IMPORT(XMACreateContext)
{
    const uint32_t context = g_memory.physical.alloc(64, 0x1000);
    std::memset(g_base + context, 0, 64);
    if (ctx.r3.u32)
        store32(ctx.r3.u32, context);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(XMAReleaseContext)
{
    g_memory.physical.free(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
