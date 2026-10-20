#pragma once

// GGPO-style rollback session for two players.
//
// The design follows Tekken6-rollback by Ekspym (https://github.com/Ekspym/Tekken6-rollback):
// predict the remote input instead of waiting for it, and when the real input disagrees,
// rewind to the first wrong frame and re-simulate up to the present without presenting the
// intermediate frames. A short automatic input delay (1 frame up to ~100 ms RTT) keeps the
// rollbacks shallow, and the prediction window is capped (5 frames by default) so the game
// waits instead of predicting further. This is an independent implementation for the
// recompiled Xbox 360 game, not a copy of that code.

#include <tt2/netplay/input.h>
#include <tt2/netplay/transport.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace tt2::netplay
{
    struct SessionConfig
    {
        int local_player = 0;          // game port of the local player (0 or 1)
        int input_delay = -1;          // frames; -1 = chosen from the measured round trip
        int max_prediction = 5;        // frames the session may run ahead of the remote input
        uint64_t build_id = 0;         // hash of game data + runtime build; must match on both sides
        int sync_roundtrips = 5;       // handshake round trips used to measure the ping
        int disconnect_notify_ms = 1000;
        int disconnect_timeout_ms = 5000;
        int checksum_interval = 60;    // frames between desync checks (0 = off)
        int frame_ms_x1000 = 16667;    // nominal frame time in microseconds (60 fps)
    };

    enum class EventType
    {
        Synchronizing,          // progress = round trips done
        Running,                // both sides synchronized, frame 0 may start
        ConnectionInterrupted,  // no packets for disconnect_notify_ms
        ConnectionResumed,
        Disconnected,           // timeout, peer quit, or incompatible build (see `reason`)
        DesyncDetected,         // checksums of a confirmed frame differ (frame = that frame)
    };

    struct Event
    {
        EventType type{};
        int frame = 0;
        int progress = 0;
        std::string reason;
    };

    struct SessionCallbacks
    {
        // Save the state at the start of `frame` (overwrites an older save of the same frame).
        // Return a checksum of the deterministic state for desync detection, or 0.
        std::function<uint64_t(int frame)> save_state;
        // Restore the state saved at the start of `frame`. Only frames within the last
        // max_prediction + 1 frames are ever requested.
        std::function<void(int frame)> load_state;
        // Run one frame with these inputs without presenting it (re-simulation).
        std::function<void(const Input (&inputs)[kMaxPlayers])> resimulate_frame;
        std::function<void(const Event&)> on_event;
    };

    struct NetworkStats
    {
        int rtt_ms = 0;
        int input_delay = 0;
        int frames_ahead = 0;       // smoothed: positive = this side runs ahead of the peer
        uint64_t rollbacks = 0;
        uint64_t resimulated_frames = 0;
        uint64_t max_rollback_depth = 0;
        uint64_t frames_waited = 0;
        uint64_t packets_sent = 0;
        uint64_t packets_received = 0;
    };

    enum class FrameResult
    {
        Run,        // `inputs` is valid: run and present the frame, then call end_frame()
        Wait,       // too far ahead of the peer (prediction window or time sync): skip this tick
        NotReady,   // still synchronizing
        Ended,      // disconnected
    };

    class Session
    {
    public:
        // `now_us` supplies a monotonic clock in microseconds (injectable for tests).
        Session(const SessionConfig& config, Transport& transport, SessionCallbacks callbacks, std::function<int64_t()> now_us = {});
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Pumps the network. Call every host tick, including while begin_frame() says Wait.
        void poll();

        // Frame boundary of a live frame. Performs any pending rollback, queues the local
        // input (applied input_delay frames later) and fills `inputs` for this frame.
        FrameResult begin_frame(Input local_input, Input (&inputs)[kMaxPlayers]);

        // The live frame ran: the state at the start of the next frame is saved.
        void end_frame();

        // Frames the session may run ahead of the remote input from now on. 0 is lockstep: a frame
        // runs only once both inputs are confirmed, so nothing is ever rolled back.
        void set_max_prediction(int frames);

        // Tells the peer we are leaving (sent a few times; the peer gets Disconnected).
        void quit();

        int frame() const;
        bool running() const;
        bool resimulating() const;
        NetworkStats stats() const;

    private:
        struct Impl;
        Impl* impl_;
    };
}
