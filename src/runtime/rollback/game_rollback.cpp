#include "game_rollback.h"
#include "snapshot.h"
#include "../audio/audio.h"
#include "../gpu/video.h"
#include "../image.h"
#include "../kernel/threads.h"
#include "../memory.h"

#include <tt2/netplay/session.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tt2
{
    uint32_t timestamp_bundle();             // image.cpp
    uint64_t timebase_now();                 // timebase.cpp
    void timebase_frame(uint64_t frame_base);
    void timebase_set_deterministic();
    uint64_t timebase_deterministic_start();
}

namespace tt2::rollback
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int kRing = 256;               // frames of recorded inputs / hashes
        constexpr int kSnapshotDepth = 8;        // frames the snapshot ring can go back
        constexpr uint64_t kTicksPerFrame = 50'000'000 / 60;

        enum class Mode
        {
            Off,
            Stress,
            Netplay,
        };

        struct FrameInput
        {
            input::PadState pads[2];
        };

        Mode g_mode = Mode::Off;
        // snapshots
        std::unique_ptr<SnapshotRing> g_ring;
        std::atomic<SnapshotRing*> g_ring_ptr{ nullptr }; // read by the fault handler
        std::mutex g_pending_mutex;
        std::vector<std::pair<uint32_t, uint32_t>> g_pending; // new allocations to track
        uint64_t g_time0 = 0;                     // guest time of frame 0

        thread_local bool t_simulation = false;   // the thread that runs the main loop

        // Write faults on write-protected game pages decide whose page it is (snapshot.h).
        // True while the ring is being replaced: a write to a page the old ring still protects
        // simply waits (retries) until that ring has made everything writable again.
        std::atomic<bool> g_ring_switching{ false };

        LONG CALLBACK write_fault_handler(EXCEPTION_POINTERS* info)
        {
            const auto* rec = info->ExceptionRecord;
            if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            SnapshotRing* ring = g_ring_ptr.load(std::memory_order_acquire);
            if (ring && ring->on_write_fault(rec->ExceptionInformation[1], t_simulation))
                return EXCEPTION_CONTINUE_EXECUTION;
            const uintptr_t addr = rec->ExceptionInformation[1];
            if (g_ring_switching.load(std::memory_order_acquire) && addr >= uintptr_t(g_base) + 0x10000 && addr < uintptr_t(g_base) + PPC_MEMORY_SIZE)
            {
                SwitchToThread();
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        FrameRegs g_regs;
        std::atomic<bool> g_resim{ false };


        // frame inputs (frozen at the boundary)
        std::array<FrameInput, kRing> g_inputs{};
        FrameInput g_current{};
        bool g_intercept = false;
        int g_frame = 0;                          // frame about to run

        // stress test
        int g_stress_interval = 0, g_stress_depth = 3, g_stress_start = 0;
        std::array<uint64_t, kRing> g_orig_hash{};
        std::array<std::vector<std::pair<uint32_t, uint64_t>>, kRing> g_orig_changes{};
        // Contents of the pages the last few original frames changed (byte-level diagnostics).
        std::array<std::map<uint32_t, std::vector<uint8_t>>, 8> g_orig_pages{};

        uint32_t load32_host(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return __builtin_bswap32(v);
        }

        void snapshot_changed_pages(int frame)
        {
            auto& m = g_orig_pages[frame % 8];
            m.clear();
            for (auto [page, h] : g_ring->last_changes())
                m[page].assign(g_base + size_t(page) * 4096, g_base + size_t(page) * 4096 + 4096);
        }

        // Up to 6 differing big-endian words of a page: original vs now.
        std::string word_diff(int frame, uint32_t page)
        {
            const auto& m = g_orig_pages[frame % 8];
            auto it = m.find(page);
            if (it == m.end())
                return " (no copy)";
            std::string out;
            int n = 0;
            for (uint32_t o = 0; o < 4096 && n < 6; o += 4)
            {
                const uint32_t a = load32_host(it->second.data() + o), b = load32(page * 4096 + o);
                if (a != b)
                {
                    char buf[48];
                    snprintf(buf, sizeof(buf), " +%03X:%08X/%08X", o, a, b);
                    out += buf;
                    n++;
                }
            }
            return out;
        }
        std::array<int, kRing> g_orig_frame{};
        uint64_t g_rollbacks = 0, g_resim_frames = 0, g_mismatches = 0;
        double g_save_ms = 0, g_load_ms = 0, g_resim_ms = 0;
        uint64_t g_saves = 0;

        // netplay
        netglue::Options g_net;
        std::unique_ptr<netplay::Transport> g_transport;
        std::unique_ptr<netplay::Session> g_session;
        bool g_frame_open = false;

        std::mutex g_status_mutex;
        std::string g_status = "offline";

        void set_status(std::string s)
        {
            std::lock_guard lock(g_status_mutex);
            g_status = std::move(s);
        }

        double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

        uint64_t frame_time(int frame) { return g_time0 + uint64_t(frame) * kTicksPerFrame; }

        // TT2_ROLLBACK_REALTIME=1 keeps the real clock on the simulation thread (diagnostics).
        bool g_realtime = [] {
            const char* v = std::getenv("TT2_ROLLBACK_REALTIME");
            return v && *v == '1';
        }();

        void lock_time(int frame)
        {
            if (!g_realtime)
                timebase_frame(frame_time(frame));
        }

        // ---------------------------------------------------------------- snapshots

        // Game state: every allocation span of the guest heaps plus the image's writable data.
        // Spans include free gaps; the ring skips pages that were never written.
        void start_snapshots(int frame)
        {
            const auto t0 = Clock::now();
            g_ring = std::make_unique<SnapshotRing>(g_base, PPC_MEMORY_SIZE, kSnapshotDepth);
            for (RangeAllocator* a : { &g_memory.virt, &g_memory.physical, &g_memory.system })
            {
                const auto r = a->ranges();
                if (r.empty())
                    continue;
                g_ring->track(r.front().first, r.back().first + r.back().second);
            }
            g_ring->track(image_data_start(), g_image.base + g_image.size); // .data, .tls and the rest

            // Not game state: the GPU ring (the null GPU's read pointer is not rolled back), the
            // kernel time stamp, and the stacks of every thread except the simulation thread.
            if (const auto [ring, bytes] = video::ring_range(); ring && bytes)
                g_ring->exclude(ring, ring + bytes);
            for (auto [b, e] : game::excluded_from_rollback())
                g_ring->exclude(b, e);
            if (const uint32_t ts = timestamp_bundle())
                g_ring->exclude(ts & ~0xFFFu, (ts & ~0xFFFu) + 0x1000);
            // Thread stacks are not game state either. The simulation thread's stack holds only
            // the main loop's own (unchanging) frames above r1 and scratch below it, which differs
            // between a frame and its re-run (re-runs start deeper, inside the hook).
            kernel::for_each_thread([&](kernel::Thread& t) {
                if (t.stack_base)
                    g_ring->exclude(t.stack_base, t.stack_base + t.stack_size);
            });

            // Allocations made from now on become game state too (applied at the next boundary).
            for (RangeAllocator* a : { &g_memory.virt, &g_memory.physical, &g_memory.system })
                a->set_on_alloc([](uint32_t start, uint32_t size) {
                    std::lock_guard lock(g_pending_mutex);
                    g_pending.push_back({ start, size });
                });

            // Publish the ring before begin() protects anything: a thread that writes a page in
            // the meantime faults into the handler and waits on the ring's lock.
            g_ring_ptr.store(g_ring.get(), std::memory_order_release);
            static bool handler_added = false;
            if (!handler_added)
            {
                handler_added = true;
                AddVectoredExceptionHandler(1, write_fault_handler);
            }
            g_ring->begin(frame);
            log("rollback: snapshots started at frame %d (%zu KB of game memory, %.0f ms)", frame, g_ring->shadow_bytes() / 1024, ms_since(t0));
        }

        void restart_snapshots(int frame)
        {
            if (g_ring)
            {
                g_ring_switching = true;
                g_ring_ptr.store(nullptr, std::memory_order_release);
                g_ring.reset(); // makes every page it protected writable again
                {
                    std::lock_guard lock(g_pending_mutex);
                    g_pending.clear();
                }
            }
            start_snapshots(frame);
            g_ring_switching = false;
        }

        void apply_pending_tracks()
        {
            std::lock_guard lock(g_pending_mutex);
            for (auto [start, size] : g_pending)
                g_ring->track(start, start + size);
            g_pending.clear();
        }

        // TT2_NET_HASHLOG=1 (desync hunting): during rounds, record which game-state pages each
        // frame changed (with a hash of their content) and write them to logs\<name>_hashes.txt.
        // The last record of a frame wins (a re-simulated frame replaces the mispredicted one).
        bool g_hashlog_round = false;
        std::map<int, std::string> g_hashlog;

        void write_hashlog()
        {
            if (g_hashlog.empty())
                return;
            const char* name = std::getenv("TT2_LOG_NAME");
            if (FILE* f = fopen((std::string("..\\logs\\") + (name ? name : "tt2") + "_hashes.txt").c_str(), "a"))
            {
                for (auto& [frame, line] : g_hashlog)
                    fprintf(f, "%d%s\n", frame, line.c_str());
                fclose(f);
            }
            g_hashlog.clear();
        }

        void save(int frame)
        {
            const auto t0 = Clock::now();
            apply_pending_tracks();
            g_ring->save(frame);
            static const bool hashlog = [] { const char* v = std::getenv("TT2_NET_HASHLOG"); return v && *v == '1'; }();
            if (hashlog && g_hashlog_round)
            {
                std::string line;
                char buf[32];
                for (auto [page, hash] : g_ring->last_changes())
                {
                    snprintf(buf, sizeof(buf), " %05X:%08X", page, uint32_t(hash));
                    line += buf;
                }
                g_hashlog[frame - 1] = line; // what frame-1 changed
                // Flush frames that can no longer be re-simulated.
                while (g_hashlog.size() > 300)
                {
                    const char* name = std::getenv("TT2_LOG_NAME");
                    if (FILE* f = fopen((std::string("..\\logs\\") + (name ? name : "tt2") + "_hashes.txt").c_str(), "a"))
                    {
                        fprintf(f, "%d%s\n", g_hashlog.begin()->first, g_hashlog.begin()->second.c_str());
                        fclose(f);
                    }
                    g_hashlog.erase(g_hashlog.begin());
                }
            }
            g_save_ms += ms_since(t0);
            g_saves++;
        }

        bool load(int frame)
        {
            const auto t0 = Clock::now();
            if (!kernel::wait_until_blocked(game::render_thread_entry(), 50))
            {
                static int logged = 0;
                if (logged++ < 5)
                    log("rollback: the render thread is still busy; restoring anyway");
            }
            const bool ok = g_ring->load(frame);
            g_load_ms += ms_since(t0);
            if (!ok)
                log("rollback: frame %d is no longer in the snapshot ring", frame);
            return ok;
        }

        // One frame without presenting or new sounds, with the given inputs.
        uint64_t g_frame_serial = 0;

        // The game's random numbers come from linear congruential generators
        // (x = x * 0x6C078965 + 12345) with their state in the image's data section: one for
        // integers, others for floats in a range. Random choices (stages, CPU opponents, intros,
        // effects) must match on both PCs and in re-simulated frames, so in netplay every
        // generator is seeded from the frame number at the start of each frame. The states are
        // found by their code pattern (lis rX,0x6C07; ori ..,0x8965; addi ..,12345 next to a
        // lwz/stw of the state), which holds for the retail game and the title update alike.
        std::vector<uint32_t> g_rng_states;

        void find_rng_states()
        {
            const uint32_t begin = g_image.base, end = g_image.base + g_image.size;
            auto word = [](uint32_t a) { return load32(a); };
            std::set<uint32_t> found;
            for (uint32_t a = begin; a + 4 <= end; a += 4)
            {
                const uint32_t w = word(a);
                if ((w >> 26) != 15 || ((w >> 16) & 31) != 0 || (w & 0xFFFF) != 0x6C07) // lis rX,0x6C07
                    continue;
                const uint32_t lo = a >= begin + 40 ? a - 40 : begin, hi = std::min(end, a + 56);
                bool ori = false, add = false;
                for (uint32_t b = lo; b < hi; b += 4)
                {
                    const uint32_t v = word(b);
                    ori = ori || ((v >> 26) == 24 && (v & 0xFFFF) == 0x8965);
                    add = add || ((v >> 26) == 14 && (v & 0xFFFF) == 0x3039);
                }
                if (!ori || !add)
                    continue;
                uint32_t lis[32] = {};
                bool has[32] = {};
                for (uint32_t b = lo; b < hi; b += 4)
                {
                    const uint32_t v = word(b);
                    if ((v >> 26) == 15 && ((v >> 16) & 31) == 0)
                    {
                        lis[(v >> 21) & 31] = v & 0xFFFF;
                        has[(v >> 21) & 31] = true;
                    }
                }
                for (uint32_t b = lo; b < hi; b += 4)
                {
                    const uint32_t v = word(b);
                    if ((v >> 26) != 32) // lwz
                        continue;
                    const uint32_t rb = (v >> 16) & 31, off = v & 0xFFFF;
                    if (!has[rb])
                        continue;
                    for (uint32_t c = lo; c < hi; c += 4)
                    {
                        const uint32_t x = word(c);
                        if ((x >> 26) == 36 && ((x >> 16) & 31) == rb && (x & 0xFFFF) == off) // stw to the same place
                            found.insert((lis[rb] << 16) + uint32_t(int32_t(int16_t(off))));
                    }
                }
            }
            g_rng_states.assign(found.begin(), found.end());
            std::string list;
            for (uint32_t s : g_rng_states)
            {
                char buf[12];
                snprintf(buf, sizeof(buf), " %08X", s);
                list += buf;
            }
            log("netplay: %zu random generators:%s", g_rng_states.size(), list.c_str());
        }

        void seed_rng(int frame)
        {
            for (size_t i = 0; i < g_rng_states.size(); i++)
            {
                uint32_t x = uint32_t(frame) * 0x9E3779B9u + uint32_t(i) * 0x85EBCA6Bu + 0x5EED;
                x ^= x >> 16;
                x *= 0x7FEB352Du;
                x ^= x >> 15;
                x *= 0x846CA68Bu;
                x ^= x >> 16;
                store32(g_rng_states[i], x);
            }
        }

        void resimulate(int frame, const FrameInput& in)
        {
            g_frame_serial++;
            if (g_mode == Mode::Netplay)
                seed_rng(frame);
            g_current = in;
            lock_time(frame);
            g_resim = true;
            audio::set_muted(true);
            game::run_frame_body(g_regs);
            audio::set_muted(false);
            g_resim = false;
        }

        // ---------------------------------------------------------------- stress mode

        void stress_boundary()
        {
            const int k = g_frame;
            if (k < g_stress_start)
            {
                // Before the start frame the game runs normally (loading, menus settling).
                if (k % 300 == 0)
                    log("rollback stress: frame %d, starting at %d", k, g_stress_start);
                g_frame = k + 1;
                return;
            }
            if (k == g_stress_start)
            {
                g_time0 = timebase_now() - uint64_t(k) * kTicksPerFrame; // frame-locked time continues from now
                g_intercept = true;
            }
            if (!g_ring)
                start_snapshots(k);
            else
            {
                save(k);
                g_orig_hash[k % kRing] = g_ring->last_delta_hash();
                g_orig_changes[k % kRing] = g_ring->last_changes();
                g_orig_frame[k % kRing] = k;
                snapshot_changed_pages(k);
            }

            if (g_stress_interval > 0 && k > g_stress_start + g_stress_depth && k % g_stress_interval == 0)
            {
                const int target = k - g_stress_depth;
                if (load(target))
                {
                    const auto t0 = Clock::now();
                    for (int f = target; f < k; f++)
                    {
                        resimulate(f, g_inputs[f % kRing]);
                        save(f + 1);
                        if (g_orig_frame[(f + 1) % kRing] == f + 1 && g_ring->last_delta_hash() != g_orig_hash[(f + 1) % kRing])
                        {
                            if (g_mismatches++ < 6)
                            {
                                // Which pages differ: only in the original, only in the re-run, or changed differently.
                                const auto& a = g_orig_changes[(f + 1) % kRing];
                                const auto& b = g_ring->last_changes();
                                std::map<uint32_t, std::pair<uint64_t, uint64_t>> diff;
                                for (auto [p, h] : a)
                                    diff[p].first = h;
                                for (auto [p, h] : b)
                                    diff[p].second = h;
                                std::string only_orig, only_rerun, different;
                                int no = 0, nr = 0, nd = 0;
                                for (auto& [p, hh] : diff)
                                {
                                    char buf[12];
                                    snprintf(buf, sizeof(buf), " %08X", p * 4096);
                                    if (!hh.second)
                                    {
                                        if (no++ < 8) only_orig += buf;
                                    }
                                    else if (!hh.first)
                                    {
                                        if (nr++ < 8) only_rerun += buf;
                                    }
                                    else if (hh.first != hh.second)
                                    {
                                        if (nd++ < 8) different += buf;
                                    }
                                }
                                log("rollback stress: frame %d differs: %d pages only in the original:%s | %d only in the re-run:%s | %d changed differently:%s",
                                    f, no, only_orig.c_str(), nr, only_rerun.c_str(), nd, different.c_str());
                                if (g_mismatches == 1)
                                    for (auto& [pg, hh] : diff)
                                        if (hh.first != hh.second && hh.first)
                                            log("    page %08X original/re-run:%s", pg * 4096, word_diff(f + 1, pg).c_str());
                            }
                        }
                        g_resim_frames++;
                    }
                    g_resim_ms += ms_since(t0);
                    g_rollbacks++;
                }
            }

            // Freeze this frame's inputs; the live frame and any re-run read the same values.
            for (int p = 0; p < 2; p++)
                g_current.pads[p] = input::read(p);
            g_inputs[k % kRing] = g_current;
            lock_time(k);
            g_frame_serial++;

            if (k > 0 && k % 600 == 0)
            {
                log("rollback stress: frame %d, %llu rollbacks (%llu frames re-run), %llu mismatching frames, %zu foreign pages, save %.2f ms/frame, load %.2f ms, re-run %.2f ms/frame",
                    k, (unsigned long long)g_rollbacks, (unsigned long long)g_resim_frames, (unsigned long long)g_mismatches, g_ring->foreign_pages(),
                    g_saves ? g_save_ms / g_saves : 0.0, g_rollbacks ? g_load_ms / g_rollbacks : 0.0,
                    g_resim_frames ? g_resim_ms / g_resim_frames : 0.0);
            }
            set_status("rollback stress: " + std::to_string(g_rollbacks) + " rollbacks, " + std::to_string(g_mismatches) + " mismatches");
            g_frame = k + 1;
        }

        // ---------------------------------------------------------------- netplay mode

        void start_session()
        {
            find_rng_states();
            netplay::SessionConfig cfg;
            cfg.local_player = g_net.local_player;
            cfg.input_delay = g_net.input_delay;
            // Lockstep outside fights: a rollback re-runs only the game's update calls, and the
            // menus react to input outside them, so predicting there would lose inputs on the side
            // that rolls back. TT2_NET_PREDICTION overrides it (testing).
            const char* pred = std::getenv("TT2_NET_PREDICTION");
            cfg.max_prediction = pred ? std::atoi(pred) : 0;
            cfg.build_id = g_net.build_id;
            cfg.checksum_interval = 0; // the per-frame delta hash also sees other threads' writes

            netplay::SessionCallbacks cb;
            // Snapshots only in battles (nothing rolls back in lockstep): restart_snapshots() at
            // each battle start.
            cb.save_state = [](int frame) -> uint64_t {
                if (g_ring)
                    save(frame);
                return 0;
            };
            cb.load_state = [](int frame) {
                if (!load(frame))
                    fatal("rollback: cannot restore frame %d", frame);
                g_frame = frame;
            };
            cb.resimulate_frame = [](const netplay::Input (&in)[2]) {
                FrameInput fi;
                for (int p = 0; p < 2; p++)
                    fi.pads[p] = input::from_netplay(in[p]);
                resimulate(g_frame, fi);
                g_frame++;
                g_resim_frames++;
            };
            cb.on_event = [](const netplay::Event& e) {
                switch (e.type)
                {
                case netplay::EventType::Running: log("netplay: connected, input delay %d frame(s)", e.progress); break;
                case netplay::EventType::Disconnected: log("netplay: disconnected (%s)", e.reason.c_str()); break;
                case netplay::EventType::ConnectionInterrupted: log("netplay: connection interrupted"); break;
                case netplay::EventType::ConnectionResumed: log("netplay: connection resumed"); break;
                default: break;
                }
            };
            g_session = std::make_unique<netplay::Session>(cfg, *g_transport, cb);
            set_status("connecting");
        }

        // Rollback only in battles. Menus and scene flow run in the game's draw pass, which a
        // re-simulated frame skips, so outside battles the session runs in lockstep. When a game
        // reaches the battle it waits (keeps exchanging netplay frames without running game
        // frames, its input flagged sync_ready) until the other game is there too; both then start
        // the battle on the same netplay frame and switch prediction on.
        // Rollback runs only while a round is being fought (the round clock runs: the players
        // have control). Everything else - menus, loading, intros, KOs - runs in lockstep, and
        // each round start is a sync point: a game that gets there first holds (keeps exchanging
        // netplay frames without running game frames, its input flagged sync_ready) until the
        // other one is there too, and both start the round with rollback on the same netplay
        // frame. Whatever drifted before (intros timed by sound, random choices) does not matter
        // to the fight: the round starts from the same positions.
        enum class Phase
        {
            Lockstep,
            Barrier,
            Round,
        };
        Phase g_phase = Phase::Lockstep;
        std::atomic<int> g_battle_calls{ 0 };
        std::atomic<int> g_round_ticks{ 0 };
        bool g_battle_seen = false; // the battle scene ran in the last live frame

        int battle_prediction()
        {
            const char* v = std::getenv("TT2_NET_PREDICTION");
            return v ? std::atoi(v) : g_net.max_prediction;
        }

        // TT2_NET_DUMP=frame[,frame...]: write the game's writable image data and virtual-memory
        // heaps at those frames (netplay frames, or main loop iterations outside netplay) to
        // native\logs: diff two players' dumps to find a desync, or search for a variable.
        void maybe_dump_memory(int frame)
        {
            static const std::vector<int> dump_frames = [] {
                std::vector<int> v;
                if (const char* s = std::getenv("TT2_NET_DUMP"))
                    for (const char* p = s; *p;)
                    {
                        v.push_back(std::atoi(p));
                        while (*p && *p != ',')
                            p++;
                        if (*p)
                            p++;
                    }
                return v;
            }();
            if (std::find(dump_frames.begin(), dump_frames.end(), frame) != dump_frames.end())
            {
                const char* name = std::getenv("TT2_LOG_NAME");
                char file[96];
                snprintf(file, sizeof(file), "%s_mem_%06d.bin", name ? name : "tt2", frame);
                if (FILE* f = fopen((std::string("..\\logs\\") + file).c_str(), "wb"))
                {
                    std::vector<std::pair<uint32_t, uint32_t>> spans = { { image_data_start(), g_image.base + g_image.size - image_data_start() } };
                    for (auto [s, n] : g_memory.virt.ranges())
                        spans.push_back({ s, n });
                    for (auto [s, n] : spans)
                    {
                        fwrite(&s, 4, 1, f);
                        fwrite(&n, 4, 1, f);
                        fwrite(g_base + s, 1, n, f);
                    }
                    fclose(f);
                    log("netplay: memory dump at frame %d written to logs\\%s", frame, file);
                }
            }
        }

        void netplay_boundary()
        {
            if (!g_session)
                start_session();

            // Work the other threads started during the frame (loading, decompression) finishes
            // before the next frame on both PCs, so loads complete on the same frame everywhere.
            kernel::wait_for_other_threads(2000);
            // In battles, audio follows the frames (the intros wait for voice lines): this frame's
            // samples, then every thread settles before the next frame. Audio memory is not game
            // state. Elsewhere audio keeps the wall clock: movies wait for it within a frame.
            if (g_battle_seen)
            {
                const bool previous = set_simulation(false);
                audio::frame_step();
                set_simulation(previous);
                kernel::wait_for_all_threads(50);
            }
            else
                audio::stop_frame_driven();

            // The previous live frame ran: the session saves the state of this frame.
            if (g_frame_open)
                g_session->end_frame();
            g_frame_open = false;

            // Did the battle scene run in the frame that just ended? The battle scene starts with
            // the VS screen while the stage and characters load; rollback waits until nothing has
            // been read from disc for 2 seconds (restoring memory under a loader would break it).
            const bool in_battle = g_battle_calls.exchange(0) > 0;
            g_battle_seen = in_battle;
            const bool ticking = g_round_ticks.exchange(0) > 0;
            static uint64_t reads_seen = 0;
            static int quiet_frames = 0;
            static int idle_frames = 0;     // frames since the round clock last ran
            static bool battle_snapshots = false;
            if (const uint64_t reads = kernel::g_file_reads.load(std::memory_order_relaxed); reads != reads_seen)
            {
                reads_seen = reads;
                quiet_frames = 0;
            }
            else
                quiet_frames++;
            idle_frames = ticking ? 0 : idle_frames + 1;

            // Fresh snapshots once the battle has loaded (which pages are game state is decided by
            // who writes them now; memory has been reused many times since boot). Taken in
            // lockstep, during the intros, so the cost does not land on the round start.
            if (!in_battle)
                battle_snapshots = false;
            else if (!battle_snapshots && quiet_frames >= 120 && g_phase == Phase::Lockstep)
            {
                battle_snapshots = true;
                restart_snapshots(g_session->frame());
            }

            if (g_phase == Phase::Lockstep && in_battle && battle_snapshots && ticking)
            {
                g_phase = Phase::Barrier;
                log("netplay: round started at frame %d, waiting for the other game", g_session->frame());
            }
            else if (g_phase == Phase::Round && (idle_frames >= 30 || !in_battle))
            {
                g_phase = Phase::Lockstep;
                g_session->set_max_prediction(0);
                g_hashlog_round = false;
                write_hashlog();
                log("netplay: round over at frame %d, back to lockstep", g_session->frame());
            }

            netplay::Input local = input::to_netplay(input::read(0));
            if (g_phase == Phase::Barrier)
                local |= netplay::button::sync_ready;
            for (;;)
            {
                netplay::Input in[2];
                const int frame = g_session->frame();
                g_frame = frame;
                const auto result = g_session->begin_frame(local, in); // may roll back and re-run frames
                if (result == netplay::FrameResult::Run && g_phase == Phase::Barrier)
                {
                    if ((in[0] & in[1] & netplay::button::sync_ready) != 0)
                    {
                        // Both games are at the round start: fight it with rollback.
                        game::align_frame_counter(frame);
                        g_phase = Phase::Round;
                        g_hashlog_round = true;
                        idle_frames = 0;
                        g_session->set_max_prediction(battle_prediction());
                        log("netplay: round runs from frame %d (rollback up to %d frames)", frame, battle_prediction());
                    }
                    else
                    {
                        // Still waiting: this netplay frame passes without a game frame.
                        g_session->end_frame();
                        continue;
                    }
                }
                if (result == netplay::FrameResult::Run)
                {
                    for (int p = 0; p < 2; p++)
                        g_current.pads[p] = input::from_netplay(in[p]);
                    static int logged = 0;
                    if ((in[0] || in[1]) && logged++ < 40)
                        log("netplay: frame %d inputs %04X %04X", g_session->frame(), unsigned(in[0]), unsigned(in[1]));
                    g_frame = g_session->frame();
                    lock_time(g_frame);
                    seed_rng(g_frame);
                    g_frame_serial++;
                    g_frame_open = true;
                    break;
                }
                if (result == netplay::FrameResult::Ended)
                {
                    log("netplay: session ended - continuing as a local game");
                    set_status("disconnected");
                    g_mode = Mode::Off;
                    g_intercept = false;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500)); // waiting for the peer
            }

            maybe_dump_memory(g_session->frame());
            if (g_session->frame() % 600 == 0)
            {
                const auto s = g_session->stats();
                log("netplay: frame %d, ping %d ms, delay %d, %llu frames waited, %llu rollbacks", g_session->frame(), s.rtt_ms, s.input_delay,
                    (unsigned long long)s.frames_waited, (unsigned long long)s.rollbacks);
            }
            if (g_session->frame() % 60 == 0)
            {
                const auto s = g_session->stats();
                set_status("ping " + std::to_string(s.rtt_ms) + " ms, delay " + std::to_string(s.input_delay) + ", rollbacks " +
                           std::to_string(s.rollbacks) + " (max " + std::to_string(s.max_rollback_depth) + " frames)");
            }
        }
    }

    void draw_pass(bool drawing)
    {
        if (g_mode != Mode::Off)
            t_simulation = !drawing;
    }

    bool set_simulation(bool simulation)
    {
        const bool previous = t_simulation;
        if (g_mode != Mode::Off)
            t_simulation = simulation;
        return previous;
    }

    void note_host_write(uint32_t address, uint32_t size)
    {
        if (SnapshotRing* ring = g_ring_ptr.load(std::memory_order_acquire))
            ring->before_host_write(address, size, t_simulation);
    }

    void frame_boundary(const FrameRegs& regs)
    {
        g_regs = regs;
        t_simulation = true;
        if (!g_time0)
            g_time0 = timebase_now();
        switch (g_mode)
        {
        case Mode::Stress: stress_boundary(); break;
        case Mode::Netplay: netplay_boundary(); break;
        case Mode::Off:
        {
            static int iterations = 0;
            maybe_dump_memory(iterations++);
            break;
        }
        }
    }

    bool resimulating() { return g_resim; }

    bool input_for_port(int port, input::PadState& out)
    {
        if (!g_intercept || port < 0 || port > 1)
            return false;
        out = g_current.pads[port];
        if (g_mode == Mode::Netplay)
            out.connected = true;
        return true;
    }

    void configure_netplay(const netglue::Options& o)
    {
        g_net = o;
        std::string error;
        g_transport = netplay::make_udp_transport(o.local_port, o.remote_host, o.remote_port, &error);
        if (!g_transport)
        {
            log("netplay: %s", error.c_str());
            set_status("error: " + error);
            return;
        }
        g_mode = Mode::Netplay;
        g_intercept = true;
        // Both games must read the same clocks from the very start (boot code seeds from them).
        timebase_set_deterministic();
        g_time0 = timebase_deterministic_start();
        log("netplay: player %d, local port %u, peer %s:%u (rollback starts at the first frame boundary)", o.local_player + 1, o.local_port,
            o.remote_host.c_str(), o.remote_port);
    }

    void configure_stress(int interval, int depth, int start)
    {
        if (g_mode == Mode::Netplay)
            return;
        g_mode = Mode::Stress;
        g_stress_interval = std::max(1, interval);
        g_stress_depth = std::clamp(depth, 1, kSnapshotDepth - 1);
        g_stress_start = std::max(0, start);
        log("rollback stress test: every %d frames, %d frames deep, from frame %d", g_stress_interval, g_stress_depth, g_stress_start);
    }

    bool netplay_active() { return g_mode == Mode::Netplay; }

    void note_battle_scene() { g_battle_calls.fetch_add(1, std::memory_order_relaxed); }

    void note_round_tick()
    {
        // Re-simulated frames tick too; only live frames count.
        if (!g_resim)
            g_round_ticks.fetch_add(1, std::memory_order_relaxed);
    }

    int current_frame(uint64_t* serial)
    {
        if (serial)
            *serial = g_frame_serial;
        return g_intercept ? g_frame : -1;
    }

    std::string status_line()
    {
        std::lock_guard lock(g_status_mutex);
        return g_status;
    }

    void shutdown()
    {
        if (g_session)
            g_session->quit();
    }
}
