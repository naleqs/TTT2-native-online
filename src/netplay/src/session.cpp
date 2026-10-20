#include <tt2/netplay/session.h>

#include "wire.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <random>

namespace tt2::netplay
{
    namespace
    {
        constexpr uint32_t kMagic = 0x4E325454; // "TT2N"
        constexpr uint8_t kProtocolVersion = 1;
        constexpr int kRing = 256;              // frames of input history kept (power of two)
        constexpr int kMaxInputsPerPacket = 64;

        enum class Msg : uint8_t
        {
            SyncRequest = 1,
            SyncReply = 2,
            SyncDone = 3,
            Input = 4,
            Checksum = 5,
            Quit = 6,
            Ping = 7,
            Pong = 8,
        };

        int ring(int frame) { return frame & (kRing - 1); }

        int64_t steady_now_us()
        {
            using namespace std::chrono;
            return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // Tekken6-rollback's choice: 1 frame of delay up to ~100 ms round trip, then one more
        // frame per extra 50 ms so rollbacks stay within the prediction window.
        int auto_delay(int rtt_ms)
        {
            if (rtt_ms <= 100)
                return 1;
            return std::min(4, 1 + (rtt_ms - 100 + 49) / 50);
        }
    }

    struct Session::Impl
    {
        enum class State
        {
            Syncing,
            Running,
            Ended,
        };

        SessionConfig cfg;
        Transport& transport;
        SessionCallbacks cb;
        std::function<int64_t()> now;
        std::mt19937 rng{ std::random_device{}() };

        State state = State::Syncing;
        int remote_player = 1;

        // handshake: requests are resent every 100 ms, so a reply may answer an older request
        // when the round trip is longer than that; remember the recent ones.
        std::map<uint32_t, int64_t> sync_pending; // nonce -> send time
        int64_t sync_sent_us = 0;
        int sync_roundtrips = 0;
        bool local_sync_done = false;
        bool remote_sync_done = false;
        int local_delay_proposal = 1;
        int remote_delay_proposal = 1;
        int64_t last_sync_done_sent_us = 0;

        // timing
        double rtt_us = 0;
        int64_t last_recv_us = 0;
        int64_t last_send_us = 0;
        int64_t last_ping_us = 0;
        bool interrupted = false;
        double frames_ahead = 0;
        int last_stall_frame = -1000;

        // frames
        int delay = 1;
        int frame = 0;              // frame the next begin_frame() simulates
        int used_upto = -1;         // highest frame simulated with recorded inputs
        int last_local = -1;        // highest frame with a local input
        int last_remote = -1;       // highest frame with a contiguous confirmed remote input
        int remote_ack = -1;        // highest local frame the peer confirmed
        int first_wrong = INT_MAX;  // earliest frame simulated with a wrong prediction
        bool resim = false;

        std::array<Input, kRing> local{};
        std::array<Input, kRing> remote{};
        std::array<Input, kRing> used{};

        // desync detection
        struct ChecksumSlot
        {
            int frame = -1;
            uint64_t hash = 0;
        };
        std::array<ChecksumSlot, kRing> checksums{};
        int next_checksum_frame = 0;
        std::map<int, uint64_t> sent_checksums;
        std::map<int, uint64_t> remote_checksums;

        NetworkStats st;

        Impl(const SessionConfig& c, Transport& t, SessionCallbacks callbacks, std::function<int64_t()> clock)
            : cfg(c), transport(t), cb(std::move(callbacks)), now(clock ? std::move(clock) : steady_now_us)
        {
            remote_player = cfg.local_player == 0 ? 1 : 0;
            next_checksum_frame = cfg.checksum_interval;
            last_recv_us = now();
            emit({ EventType::Synchronizing, 0, 0, {} });
        }

        void emit(const Event& e)
        {
            if (cb.on_event)
                cb.on_event(e);
        }

        void end(const std::string& reason)
        {
            if (state == State::Ended)
                return;
            state = State::Ended;
            emit({ EventType::Disconnected, frame, 0, reason });
        }

        // ------------------------------------------------------------------ sending

        void send(const wire::Writer& w)
        {
            transport.send(w.data(), w.size());
            last_send_us = now();
            st.packets_sent++;
        }

        wire::Writer header(Msg type)
        {
            wire::Writer w;
            w.u32(kMagic);
            w.u8(kProtocolVersion);
            w.u8(uint8_t(type));
            return w;
        }

        void send_sync_request()
        {
            const uint32_t nonce = rng();
            sync_sent_us = now();
            sync_pending[nonce] = sync_sent_us;
            while (sync_pending.size() > 32)
            {
                auto oldest = std::min_element(sync_pending.begin(), sync_pending.end(), [](auto& a, auto& b) { return a.second < b.second; });
                sync_pending.erase(oldest);
            }
            auto w = header(Msg::SyncRequest);
            w.u32(nonce);
            w.u64(cfg.build_id);
            w.u8(uint8_t(cfg.local_player));
            send(w);
        }

        void send_sync_done()
        {
            auto w = header(Msg::SyncDone);
            w.u8(uint8_t(local_delay_proposal));
            w.u16(uint16_t(std::min(65535.0, rtt_us / 1000)));
            send(w);
            last_sync_done_sent_us = now();
        }

        void send_inputs()
        {
            const int first = std::max(remote_ack + 1, 0);
            const int count = std::min(last_local - first + 1, kMaxInputsPerPacket);
            auto w = header(Msg::Input);
            w.i32(last_remote);
            w.i32(frame);
            w.i32(first);
            w.u8(uint8_t(std::max(count, 0)));
            for (int i = 0; i < count; i++)
                w.u32(local[ring(first + i)]);
            send(w);
        }

        void send_checksum(int f, uint64_t hash)
        {
            auto w = header(Msg::Checksum);
            w.i32(f);
            w.u64(hash);
            send(w);
        }

        // ------------------------------------------------------------------ receiving

        void poll()
        {
            if (state == State::Ended)
                return;

            std::vector<uint8_t> packet;
            while (transport.receive(packet))
            {
                wire::Reader r(packet.data(), packet.size());
                if (r.u32() != kMagic || r.u8() != kProtocolVersion || !r.ok())
                    continue;
                st.packets_received++;
                last_recv_us = now();
                if (interrupted)
                {
                    interrupted = false;
                    emit({ EventType::ConnectionResumed, frame, 0, {} });
                }
                handle(Msg(r.u8()), r);
                if (state == State::Ended)
                    return;
            }

            if (!transport.open())
            {
                end("connection closed");
                return;
            }

            const int64_t t = now();
            const int64_t silent_ms = (t - last_recv_us) / 1000;
            if (silent_ms > cfg.disconnect_timeout_ms)
            {
                end("peer timed out");
                return;
            }
            if (state == State::Running && !interrupted && silent_ms > cfg.disconnect_notify_ms)
            {
                interrupted = true;
                emit({ EventType::ConnectionInterrupted, frame, 0, {} });
            }

            if (state == State::Syncing)
            {
                if (!local_sync_done && t - sync_sent_us > 100'000)
                    send_sync_request(); // lost: try again
                if (local_sync_done && t - last_sync_done_sent_us > 100'000)
                    send_sync_done();
            }
            else if (state == State::Running)
            {
                if (t - last_ping_us > 500'000)
                {
                    auto w = header(Msg::Ping);
                    w.u64(uint64_t(t));
                    send(w);
                    last_ping_us = t;
                }
                if (t - last_send_us > 16'000)
                    send_inputs(); // keepalive and redundancy while the game waits
            }
        }

        void add_rtt_sample(int64_t sample_us)
        {
            rtt_us = rtt_us == 0 ? double(sample_us) : rtt_us * 0.875 + double(sample_us) * 0.125;
        }

        void handle(Msg type, wire::Reader& r)
        {
            switch (type)
            {
            case Msg::SyncRequest:
            case Msg::SyncReply:
            {
                const uint32_t nonce = r.u32();
                const uint64_t build = r.u64();
                const int player = r.u8();
                if (!r.ok())
                    return;
                if (build != cfg.build_id)
                {
                    end("the other player has a different game version or build");
                    return;
                }
                if (player == cfg.local_player)
                {
                    end("both players picked the same side");
                    return;
                }
                if (type == Msg::SyncRequest)
                {
                    auto w = header(Msg::SyncReply);
                    w.u32(nonce);
                    w.u64(cfg.build_id);
                    w.u8(uint8_t(cfg.local_player));
                    send(w);
                    if (state == State::Running)
                        send_sync_done(); // the peer is still waiting for our SyncDone
                    return;
                }
                const auto pending = sync_pending.find(nonce);
                if (state != State::Syncing || local_sync_done || pending == sync_pending.end())
                    return;
                add_rtt_sample(now() - pending->second);
                sync_pending.erase(pending);
                sync_roundtrips++;
                emit({ EventType::Synchronizing, 0, sync_roundtrips, {} });
                if (sync_roundtrips >= cfg.sync_roundtrips)
                {
                    local_sync_done = true;
                    local_delay_proposal = cfg.input_delay >= 0 ? cfg.input_delay : auto_delay(int(rtt_us / 1000));
                    send_sync_done();
                    maybe_start();
                }
                else
                {
                    send_sync_request();
                }
                return;
            }

            case Msg::SyncDone:
            {
                const int proposal = r.u8();
                r.u16();
                if (!r.ok())
                    return;
                if (state == State::Running)
                {
                    send_sync_done();
                    return;
                }
                remote_sync_done = true;
                remote_delay_proposal = proposal;
                maybe_start();
                return;
            }

            case Msg::Input:
            {
                const int ack = r.i32();
                const int sender_frame = r.i32();
                const int first = r.i32();
                const int count = r.u8();
                if (!r.ok())
                    return;
                remote_ack = std::max(remote_ack, std::min(ack, last_local));
                for (int i = 0; i < count; i++)
                {
                    const Input in = r.u32() & button::all_valid;
                    if (!r.ok())
                        return;
                    const int f = first + i;
                    if (f != last_remote + 1)
                        continue; // duplicate, or a gap (an older packet will fill it)
                    if (f - frame >= kRing / 2)
                        break; // peer far ahead; should not happen with time sync
                    remote[ring(f)] = in;
                    last_remote = f;
                    if (f <= used_upto && used[ring(f)] != in)
                        first_wrong = std::min(first_wrong, f);
                }
                if (state == State::Running && rtt_us > 0)
                {
                    const double remote_now = sender_frame + (rtt_us / 2) / cfg.frame_ms_x1000;
                    const double sample = frame - remote_now;
                    frames_ahead = frames_ahead * 0.9 + sample * 0.1;
                }
                return;
            }

            case Msg::Checksum:
            {
                const int f = r.i32();
                const uint64_t hash = r.u64();
                if (!r.ok())
                    return;
                remote_checksums[f] = hash;
                compare_checksums();
                return;
            }

            case Msg::Quit:
                end("the other player left");
                return;

            case Msg::Ping:
            {
                const uint64_t t = r.u64();
                if (!r.ok())
                    return;
                auto w = header(Msg::Pong);
                w.u64(t);
                send(w);
                return;
            }

            case Msg::Pong:
            {
                const uint64_t t = r.u64();
                if (r.ok() && int64_t(t) <= now())
                    add_rtt_sample(now() - int64_t(t));
                return;
            }
            }
        }

        void maybe_start()
        {
            if (state != State::Syncing || !local_sync_done || !remote_sync_done)
                return;

            delay = std::max(local_delay_proposal, remote_delay_proposal);
            state = State::Running;
            last_ping_us = now();

            // The first `delay` frames have no real local input yet: neutral, and already
            // "confirmed" so the peer can simulate them.
            for (int f = 0; f < delay; f++)
                local[ring(f)] = 0;
            last_local = delay - 1;

            emit({ EventType::Running, 0, delay, {} });
            store_checksum(0, cb.save_state ? cb.save_state(0) : 0);
            send_inputs();
        }

        // ------------------------------------------------------------------ frames

        Input remote_input(int f) const
        {
            if (f <= last_remote)
                return remote[ring(f)];
            return last_remote >= 0 ? remote[ring(last_remote)] : 0; // predict: repeat last
        }

        void inputs_for(int f, Input (&out)[kMaxPlayers])
        {
            out[cfg.local_player] = local[ring(f)];
            out[remote_player] = remote_input(f);
            used[ring(f)] = out[remote_player];
            used_upto = std::max(used_upto, f);
        }

        void store_checksum(int f, uint64_t hash)
        {
            checksums[ring(f)] = { f, hash };
        }

        void rollback()
        {
            if (first_wrong >= frame)
            {
                first_wrong = INT_MAX;
                return;
            }

            const int target = first_wrong;
            first_wrong = INT_MAX;
            const uint64_t depth = uint64_t(frame - target);

            cb.load_state(target);
            resim = true;
            for (int f = target; f < frame; f++)
            {
                Input in[kMaxPlayers];
                inputs_for(f, in);
                cb.resimulate_frame(in);
                store_checksum(f + 1, cb.save_state ? cb.save_state(f + 1) : 0);
            }
            resim = false;

            st.rollbacks++;
            st.resimulated_frames += depth;
            st.max_rollback_depth = std::max(st.max_rollback_depth, depth);
        }

        // A frame's state is final once every input before it is confirmed and no rollback
        // before it is pending.
        void flush_checksums()
        {
            if (cfg.checksum_interval <= 0)
                return;
            const int final_upto = std::min({ last_remote + 1, frame, first_wrong });
            while (next_checksum_frame <= final_upto)
            {
                const auto& slot = checksums[ring(next_checksum_frame)];
                if (slot.frame == next_checksum_frame && slot.hash != 0)
                {
                    send_checksum(slot.frame, slot.hash);
                    sent_checksums[slot.frame] = slot.hash;
                }
                next_checksum_frame += cfg.checksum_interval;
            }
            compare_checksums();
        }

        void compare_checksums()
        {
            for (auto it = sent_checksums.begin(); it != sent_checksums.end();)
            {
                auto r = remote_checksums.find(it->first);
                if (r == remote_checksums.end())
                {
                    ++it;
                    continue;
                }
                if (r->second != it->second)
                    emit({ EventType::DesyncDetected, it->first, 0, {} });
                remote_checksums.erase(r);
                it = sent_checksums.erase(it);
            }
            while (remote_checksums.size() > 64)
                remote_checksums.erase(remote_checksums.begin());
            while (sent_checksums.size() > 64)
                sent_checksums.erase(sent_checksums.begin());
        }

        FrameResult begin_frame(Input local_input, Input (&out)[kMaxPlayers])
        {
            poll();
            if (state == State::Ended)
                return FrameResult::Ended;
            if (state == State::Syncing)
                return FrameResult::NotReady;

            rollback();
            flush_checksums();

            // Time sync: the side that runs ahead gives up a frame now and then.
            if (frames_ahead >= 1.0 && frame - last_stall_frame >= 20)
            {
                last_stall_frame = frame;
                frames_ahead -= 1.0;
                st.frames_waited++;
                return FrameResult::Wait;
            }

            if (frame + delay > last_local)
            {
                last_local = frame + delay;
                local[ring(last_local)] = local_input & button::all_valid;
                send_inputs();
            }

            if (frame - last_remote > cfg.max_prediction)
            {
                st.frames_waited++;
                return FrameResult::Wait;
            }

            inputs_for(frame, out);
            return FrameResult::Run;
        }

        void end_frame()
        {
            if (state != State::Running)
                return;
            frame++;
            store_checksum(frame, cb.save_state ? cb.save_state(frame) : 0);
        }
    };

    Session::Session(const SessionConfig& config, Transport& transport, SessionCallbacks callbacks, std::function<int64_t()> now_us)
        : impl_(new Impl(config, transport, std::move(callbacks), std::move(now_us)))
    {
        impl_->send_sync_request();
    }

    Session::~Session()
    {
        delete impl_;
    }

    void Session::poll() { impl_->poll(); }

    FrameResult Session::begin_frame(Input local_input, Input (&inputs)[kMaxPlayers])
    {
        return impl_->begin_frame(local_input, inputs);
    }

    void Session::end_frame() { impl_->end_frame(); }

    void Session::set_max_prediction(int frames) { impl_->cfg.max_prediction = std::max(0, frames); }

    void Session::quit()
    {
        if (impl_->state == Impl::State::Ended)
            return;
        for (int i = 0; i < 3; i++)
            impl_->send(impl_->header(Msg::Quit));
        impl_->state = Impl::State::Ended;
    }

    int Session::frame() const { return impl_->frame; }
    bool Session::running() const { return impl_->state == Impl::State::Running; }
    bool Session::resimulating() const { return impl_->resim; }

    NetworkStats Session::stats() const
    {
        NetworkStats s = impl_->st;
        s.rtt_ms = int(impl_->rtt_us / 1000);
        s.input_delay = impl_->delay;
        s.frames_ahead = int(std::lround(impl_->frames_ahead));
        return s;
    }
}
