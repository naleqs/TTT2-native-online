#include "netplay_glue.h"
#include "guest.h"

#include <tt2/netplay/session.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace tt2::netglue
{
    namespace
    {
        Options g_options;
        std::unique_ptr<netplay::Transport> g_transport;
        std::unique_ptr<netplay::Session> g_session;
        std::atomic<bool> g_active{ false };
        std::mutex g_mutex;

        std::atomic<uint64_t> g_presented{ 0 };
        uint64_t g_boundary_present = ~0ull;
        bool g_frame_open = false;
        netplay::Input g_inputs[2]{};
        std::mutex g_status_mutex; // separate: the game thread may block in boundary()
        std::string g_status = "offline";

        void set_status(std::string s)
        {
            std::lock_guard lock(g_status_mutex);
            g_status = std::move(s);
        }

        void stop(const std::string& why)
        {
            log("netplay: %s - continuing as a local game", why.c_str());
            set_status("disconnected: " + why);
            g_active = false;
        }

        // Frame boundary: close the previous frame, open this one. Blocks while the session
        // waits for the peer (that is the "wait" in delay/lockstep netcode).
        void boundary()
        {
            if (g_frame_open)
                g_session->end_frame();
            g_frame_open = false;

            const netplay::Input local = input::to_netplay(input::read(0));
            for (;;)
            {
                netplay::Input in[2];
                switch (g_session->begin_frame(local, in))
                {
                case netplay::FrameResult::Run:
                    g_inputs[0] = in[0];
                    g_inputs[1] = in[1];
                    g_frame_open = true;
                    return;
                case netplay::FrameResult::Ended:
                    stop("connection ended");
                    return;
                case netplay::FrameResult::Wait:
                case netplay::FrameResult::NotReady:
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    break;
                }
            }
        }
    }

    void configure(const Options& o)
    {
        std::lock_guard lock(g_mutex);
        g_options = o;
        if (!o.enabled)
            return;

        std::string error;
        g_transport = netplay::make_udp_transport(o.local_port, o.remote_host, o.remote_port, &error);
        if (!g_transport)
        {
            log("netplay: %s", error.c_str());
            set_status("error: " + error);
            return;
        }

        netplay::SessionConfig cfg;
        cfg.local_player = o.local_player;
        cfg.input_delay = o.input_delay;
        cfg.build_id = o.build_id;
        // No main-loop hook yet: frames cannot be re-run, so never predict (lockstep).
        cfg.max_prediction = 0;
        cfg.checksum_interval = 0;

        netplay::SessionCallbacks cb;
        cb.save_state = [](int) -> uint64_t { return 0; };
        cb.load_state = [](int frame) { fatal("netplay: rollback to frame %d without a frame hook", frame); };
        cb.resimulate_frame = [](const netplay::Input(&)[2]) {};
        cb.on_event = [](const netplay::Event& e) {
            switch (e.type)
            {
            case netplay::EventType::Running:
                log("netplay: connected, input delay %d frame(s)", e.progress);
                break;
            case netplay::EventType::Disconnected:
                log("netplay: disconnected (%s)", e.reason.c_str());
                break;
            case netplay::EventType::ConnectionInterrupted:
                log("netplay: connection interrupted");
                break;
            case netplay::EventType::DesyncDetected:
                log("netplay: DESYNC at frame %d", e.frame);
                break;
            default:
                break;
            }
        };
        g_session = std::make_unique<netplay::Session>(cfg, *g_transport, cb);
        g_active = true;
        set_status("connecting");
        log("netplay: player %d, local port %u, peer %s:%u", o.local_player + 1, o.local_port, o.remote_host.c_str(), o.remote_port);
    }

    bool active() { return g_active; }

    bool input_for_port(int port, input::PadState& out)
    {
        if (!g_active || port < 0 || port > 1)
            return false;
        std::lock_guard lock(g_mutex);
        if (!g_active)
            return false;
        // First port-0 read after a presented frame starts the next netplay frame.
        if (port == 0 && g_presented != g_boundary_present)
        {
            g_boundary_present = g_presented;
            boundary();
            if (!g_active)
                return false;
            const auto s = g_session->stats();
            set_status("ping " + std::to_string(s.rtt_ms) + " ms, delay " + std::to_string(s.input_delay) + ", frame " + std::to_string(g_session->frame()));
        }
        out = input::from_netplay(g_inputs[port]);
        return true;
    }

    void on_present() { g_presented++; }

    std::string status_line()
    {
        std::lock_guard lock(g_status_mutex);
        return g_status;
    }

    void shutdown()
    {
        std::lock_guard lock(g_mutex);
        if (g_session)
            g_session->quit();
        g_session.reset();
        g_transport.reset();
        g_active = false;
    }
}
