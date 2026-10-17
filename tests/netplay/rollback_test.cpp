// Rollback session test: two sessions over a simulated network (latency, jitter, loss,
// reordering, drifting clocks) driving a deterministic toy game. Every confirmed frame must
// end up with the same state on both sides, and every applied input must be the input the
// owning player actually pressed `input_delay` frames earlier.

#include <tt2/netplay/session.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace tt2::netplay;

namespace
{
    int g_failures = 0;

#define CHECK(cond, ...)                                       \
    do                                                         \
    {                                                          \
        if (!(cond))                                           \
        {                                                      \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            g_failures++;                                      \
        }                                                      \
    } while (0)

    struct NetParams
    {
        int latency_ms = 20;
        int jitter_ms = 0;
        double loss = 0.0;
    };

    struct Pipe
    {
        std::multimap<int64_t, std::vector<uint8_t>> queue; // deliver time -> datagram
        bool cut = false;
    };

    class SimTransport final : public Transport
    {
    public:
        SimTransport(Pipe& out, Pipe& in, const int64_t& clock, NetParams params, uint32_t seed)
            : out_(out), in_(in), clock_(clock), params_(params), rng_(seed) {}

        void send(const uint8_t* data, size_t size) override
        {
            if (out_.cut || std::uniform_real_distribution<>(0, 1)(rng_) < params_.loss)
                return;
            int64_t delay = int64_t(params_.latency_ms) * 1000;
            if (params_.jitter_ms > 0)
                delay += std::uniform_int_distribution<int64_t>(0, params_.jitter_ms * 1000)(rng_);
            out_.queue.emplace(clock_ + delay, std::vector<uint8_t>(data, data + size));
        }

        bool receive(std::vector<uint8_t>& out) override
        {
            if (in_.queue.empty() || in_.queue.begin()->first > clock_)
                return false;
            out = std::move(in_.queue.begin()->second);
            in_.queue.erase(in_.queue.begin());
            return true;
        }

    private:
        Pipe& out_;
        Pipe& in_;
        const int64_t& clock_;
        NetParams params_;
        std::mt19937 rng_;
    };

    // Deterministic toy game: positions driven by the d-pad and a running hash of everything.
    struct GameState
    {
        uint64_t hash = 0x9E3779B97F4A7C15ull;
        int32_t x[2]{};
        int32_t frame = 0;
    };

    uint64_t mix(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xFF51AFD7ED558CCDull;
    }

    void step(GameState& s, const Input (&in)[2])
    {
        for (int p = 0; p < 2; p++)
        {
            if (in[p] & button::dpad_left)
                s.x[p]--;
            if (in[p] & button::dpad_right)
                s.x[p]++;
            if (in[p] & button::a)
                s.x[p] *= 3;
        }
        s.hash = mix(mix(mix(s.hash, in[0]), in[1] * 31), uint64_t(uint32_t(s.x[0])) << 32 | uint32_t(s.x[1]));
        s.frame++;
    }

    Input scripted_input(int player, int frame)
    {
        if (frame < 0)
            return 0;
        // Holds each input for 1-8 frames so prediction is sometimes right, sometimes wrong.
        uint64_t h = mix(uint64_t(player) * 1000003, uint64_t(frame / (1 + (frame / 37) % 8)));
        return Input(h) & (button::dpad_left | button::dpad_right | button::a | button::b | button::x);
    }

    struct Side
    {
        int player = 0;
        int64_t frame_us = 16667;
        int64_t next_tick = 0;
        GameState game;
        std::map<int, GameState> saves;
        std::map<int, uint64_t> final_hash;      // state at start of frame, final timeline
        std::map<int, Input> applied_local;      // local input applied at frame, final timeline
        std::map<int, Input> applied_remote;
        std::vector<Event> events;
        int frames_run = 0;
        std::unique_ptr<SimTransport> transport;
        std::unique_ptr<Session> session;
        int delay = -1;

        bool has(EventType t) const
        {
            for (auto& e : events)
                if (e.type == t)
                    return true;
            return false;
        }
    };

    void make_session(Side& s, SessionConfig cfg, const int64_t& clock)
    {
        cfg.local_player = s.player;
        SessionCallbacks cb;
        cb.save_state = [&s](int frame) {
            s.saves[frame] = s.game;
            s.saves.erase(s.saves.begin(), s.saves.lower_bound(frame - 64));
            s.final_hash[frame] = s.game.hash;
            return s.game.hash | 1;
        };
        cb.load_state = [&s](int frame) {
            auto it = s.saves.find(frame);
            if (it == s.saves.end())
            {
                std::printf("  FAIL load of unsaved frame %d\n", frame);
                g_failures++;
                return;
            }
            s.game = it->second;
        };
        cb.resimulate_frame = [&s](const Input (&in)[2]) {
            s.applied_local[s.game.frame] = in[s.player];
            s.applied_remote[s.game.frame] = in[1 - s.player];
            step(s.game, in);
        };
        cb.on_event = [&s](const Event& e) {
            s.events.push_back(e);
            if (std::getenv("TT2_TEST_VERBOSE"))
                std::printf("    [p%d] event %d frame %d progress %d %s\n", s.player, int(e.type), e.frame, e.progress, e.reason.c_str());
            if (e.type == EventType::Running)
                s.delay = e.progress;
        };
        s.session = std::make_unique<Session>(cfg, *s.transport, cb, [&clock] { return clock; });
    }

    struct Result
    {
        Side a, b;
    };

    // Runs both sides on one virtual clock with 250 us resolution.
    void run(Side& a, Side& b, int64_t& clock, int target_frames, int64_t time_limit_us, Pipe* cut_pipe = nullptr, int cut_at_frame = -1)
    {
        Side* sides[2] = { &a, &b };
        while (clock < time_limit_us)
        {
            clock += 250;
            for (Side* s : sides)
            {
                s->session->poll();
                if (clock < s->next_tick)
                    continue;
                s->next_tick += s->frame_us;

                Input in[2];
                const int frame = s->session->frame();
                const Input local = scripted_input(s->player, frame);
                if (s->session->begin_frame(local, in) == FrameResult::Run)
                {
                    if (s->game.frame != frame)
                    {
                        std::printf("  FAIL game frame %d != session frame %d\n", s->game.frame, frame);
                        g_failures++;
                    }
                    s->applied_local[frame] = in[s->player];
                    s->applied_remote[frame] = in[1 - s->player];
                    step(s->game, in);
                    s->session->end_frame();
                    s->frames_run++;
                }
            }
            if (cut_pipe && a.session->frame() >= cut_at_frame)
                cut_pipe->cut = true;
            if (a.session->frame() >= target_frames && b.session->frame() >= target_frames)
                break;
            if (a.has(EventType::Disconnected) && b.has(EventType::Disconnected))
                break;
        }
    }

    void verify(const char* name, Side& a, Side& b, int max_prediction)
    {
        const int common = std::min(a.session->frame(), b.session->frame()) - max_prediction - 4;
        CHECK(common > 600, "%s: only %d common frames", name, common);
        CHECK(a.delay == b.delay && a.delay >= 0, "%s: delays differ %d/%d", name, a.delay, b.delay);

        int mismatches = 0;
        for (int f = 0; f < common; f++)
        {
            if (a.final_hash[f] != b.final_hash[f])
            {
                if (mismatches++ < 3)
                    std::printf("  FAIL %s: state differs at frame %d\n", name, f);
            }
            for (Side* s : { &a, &b })
            {
                Side& other = s == &a ? b : a;
                const Input want_local = f < s->delay ? 0 : scripted_input(s->player, f - s->delay);
                if (s->applied_local[f] != want_local || other.applied_remote[f] != want_local)
                {
                    if (mismatches++ < 3)
                        std::printf("  FAIL %s: player %d input wrong at frame %d\n", name, s->player, f);
                }
            }
        }
        g_failures += mismatches;
        CHECK(!a.has(EventType::DesyncDetected) && !b.has(EventType::DesyncDetected), "%s: desync reported", name);

        const auto sa = a.session->stats(), sb = b.session->stats();
        std::printf("  %-26s frames %5d/%5d  delay %d  rtt %3d ms  rollbacks %5llu/%5llu  max depth %llu/%llu  waits %llu/%llu\n",
            name, a.session->frame(), b.session->frame(), a.delay, sa.rtt_ms,
            (unsigned long long)sa.rollbacks, (unsigned long long)sb.rollbacks,
            (unsigned long long)sa.max_rollback_depth, (unsigned long long)sb.max_rollback_depth,
            (unsigned long long)sa.frames_waited, (unsigned long long)sb.frames_waited);
        CHECK(int(sa.max_rollback_depth) <= max_prediction && int(sb.max_rollback_depth) <= max_prediction, "%s: rollback deeper than the window", name);
    }

    void scenario(const char* name, NetParams net, int64_t frame_us_b = 16667, int forced_delay = -1, bool expect_rollbacks = true)
    {
        int64_t clock = 0;
        Pipe ab, ba;
        Side a, b;
        a.player = 0;
        b.player = 1;
        b.frame_us = frame_us_b;
        b.next_tick = 3000; // start out of phase
        a.transport = std::make_unique<SimTransport>(ab, ba, clock, net, 1);
        b.transport = std::make_unique<SimTransport>(ba, ab, clock, net, 2);

        SessionConfig cfg;
        cfg.build_id = 0x1234;
        cfg.input_delay = forced_delay;
        make_session(a, cfg, clock);
        make_session(b, cfg, clock);

        run(a, b, clock, 3600, 120'000'000);
        verify(name, a, b, cfg.max_prediction);
        if (expect_rollbacks)
            CHECK(a.session->stats().rollbacks + b.session->stats().rollbacks > 0, "%s: no rollbacks happened, the test proves nothing", name);
    }

    void test_build_mismatch()
    {
        int64_t clock = 0;
        Pipe ab, ba;
        Side a, b;
        a.player = 0;
        b.player = 1;
        a.transport = std::make_unique<SimTransport>(ab, ba, clock, NetParams{}, 1);
        b.transport = std::make_unique<SimTransport>(ba, ab, clock, NetParams{}, 2);
        SessionConfig cfg;
        cfg.build_id = 1;
        make_session(a, cfg, clock);
        cfg.build_id = 2;
        make_session(b, cfg, clock);
        run(a, b, clock, 100, 10'000'000);
        CHECK(a.has(EventType::Disconnected) && b.has(EventType::Disconnected), "build mismatch was not rejected");
        CHECK(!a.has(EventType::Running), "mismatched session started");
        std::printf("  %-26s rejected\n", "build mismatch");
    }

    void test_disconnect()
    {
        int64_t clock = 0;
        Pipe ab, ba;
        Side a, b;
        a.player = 0;
        b.player = 1;
        a.transport = std::make_unique<SimTransport>(ab, ba, clock, NetParams{ 30, 5, 0.02 }, 1);
        b.transport = std::make_unique<SimTransport>(ba, ab, clock, NetParams{ 30, 5, 0.02 }, 2);
        SessionConfig cfg;
        cfg.build_id = 7;
        make_session(a, cfg, clock);
        make_session(b, cfg, clock);

        // Cut B -> A after 300 frames: A must report the interruption, then the disconnect,
        // and must never simulate more than max_prediction frames past B's last input.
        run(a, b, clock, 100000, 30'000'000, &ba, 300);
        CHECK(a.has(EventType::ConnectionInterrupted), "no interruption event");
        CHECK(a.has(EventType::Disconnected), "no disconnect event");
        CHECK(a.session->frame() < 300 + 40, "A kept running without input: frame %d", a.session->frame());
        std::printf("  %-26s A stopped at frame %d and disconnected\n", "peer goes silent", a.session->frame());
    }
}

int main()
{
    std::printf("rollback session tests\n");
    scenario("LAN 2 ms", { 2, 0, 0.0 }, 16667, -1, false);
    scenario("40 ms, 1% loss", { 20, 4, 0.01 });
    scenario("100 ms, jitter, 5% loss", { 50, 15, 0.05 });
    scenario("160 ms, jitter, 10% loss", { 80, 20, 0.10 });
    scenario("clock drift 1%", { 30, 5, 0.02 }, 16500);
    scenario("forced delay 0", { 25, 5, 0.02 }, 16667, 0);
    test_build_mismatch();
    test_disconnect();

    if (g_failures)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
