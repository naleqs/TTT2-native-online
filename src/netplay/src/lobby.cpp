#include <tt2/netplay/lobby.h>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <cstdio>
#include <set>
#include <thread>

namespace tt2::lobby
{
    using json = nlohmann::json;

    // Profile pictures travel as base64 inside the JSON control messages.
    std::string base64_encode(const std::string& in)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        for (size_t i = 0; i < in.size(); i += 3)
        {
            uint32_t v = uint32_t(uint8_t(in[i])) << 16;
            if (i + 1 < in.size())
                v |= uint32_t(uint8_t(in[i + 1])) << 8;
            if (i + 2 < in.size())
                v |= uint8_t(in[i + 2]);
            out += t[(v >> 18) & 63];
            out += t[(v >> 12) & 63];
            out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
            out += i + 2 < in.size() ? t[v & 63] : '=';
        }
        return out;
    }

    std::string base64_decode(const std::string& in)
    {
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        uint32_t acc = 0;
        int bits = 0;
        for (char c : in)
        {
            const int v = value(c);
            if (v < 0)
                continue;
            acc = (acc << 6) | uint32_t(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out += char((acc >> bits) & 0xFF);
            }
        }
        return out;
    }

    constexpr size_t kMaxAvatarBytes = 64 * 1024;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        constexpr int kOffersPerAnnounce = 4;
        constexpr auto kAnnounceInterval = std::chrono::seconds(30);
        constexpr auto kPingInterval = std::chrono::seconds(2);
        constexpr auto kOfferLifetime = std::chrono::seconds(60);

        std::string random_id(std::mt19937& rng)
        {
            static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
            std::string s(20, '0');
            for (char& c : s)
                c = alphabet[rng() % (sizeof(alphabet) - 1)];
            return s;
        }

        // SHA-1 (only for the 20-character room hash; not security relevant).
        std::string sha1_hex(const std::string& input)
        {
            uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
            std::string msg = input;
            const uint64_t bits = uint64_t(input.size()) * 8;
            msg += char(0x80);
            while (msg.size() % 64 != 56)
                msg += char(0);
            for (int i = 7; i >= 0; i--)
                msg += char(bits >> (i * 8));
            auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
            for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
            {
                uint32_t w[80];
                for (int i = 0; i < 16; i++)
                    w[i] = uint32_t(uint8_t(msg[chunk + i * 4])) << 24 | uint32_t(uint8_t(msg[chunk + i * 4 + 1])) << 16 |
                           uint32_t(uint8_t(msg[chunk + i * 4 + 2])) << 8 | uint32_t(uint8_t(msg[chunk + i * 4 + 3]));
                for (int i = 16; i < 80; i++)
                    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; i++)
                {
                    uint32_t f, k;
                    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                    const uint32_t t = rol(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rol(b, 30); b = a; a = t;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }
            char out[41];
            for (int i = 0; i < 5; i++)
                snprintf(out + i * 8, 9, "%08x", h[i]);
            return std::string(out, 40);
        }

        rtc::Configuration rtc_config(const std::vector<std::string>& ice)
        {
            rtc::Configuration c;
            for (const auto& s : ice)
            {
                try
                {
                    c.iceServers.emplace_back(s);
                }
                catch (...)
                {
                    // malformed entry from .env: skip it
                }
            }
            return c;
        }

        // Datagram transport over an unreliable, unordered WebRTC data channel.
        class ChannelTransport final : public netplay::Transport
        {
        public:
            explicit ChannelTransport(std::shared_ptr<rtc::DataChannel> dc) : dc_(std::move(dc))
            {
                state_ = std::make_shared<State>();
                auto state = state_;
                dc_->onMessage([state](rtc::message_variant m) {
                    if (auto* bin = std::get_if<rtc::binary>(&m))
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->queue.size() < 1024)
                            state->queue.emplace_back(reinterpret_cast<const uint8_t*>(bin->data()), reinterpret_cast<const uint8_t*>(bin->data()) + bin->size());
                    }
                });
                dc_->onClosed([state] { state->closed = true; });
            }

            ~ChannelTransport() override
            {
                dc_->onMessage(nullptr);
                dc_->onClosed(nullptr);
            }

            void send(const uint8_t* data, size_t size) override
            {
                if (dc_->isOpen())
                {
                    try
                    {
                        dc_->send(reinterpret_cast<const std::byte*>(data), size);
                    }
                    catch (...)
                    {
                    }
                }
            }

            bool receive(std::vector<uint8_t>& out) override
            {
                std::lock_guard lock(state_->mutex);
                if (state_->queue.empty())
                    return false;
                out = std::move(state_->queue.front());
                state_->queue.pop_front();
                return true;
            }

            bool open() const override { return !state_->closed; }

        private:
            struct State
            {
                std::mutex mutex;
                std::deque<std::vector<uint8_t>> queue;
                std::atomic<bool> closed{ false };
            };
            std::shared_ptr<rtc::DataChannel> dc_;
            std::shared_ptr<State> state_;
        };
    }

    std::vector<std::string> Lobby::default_trackers()
    {
        return {
            "wss://tracker.openwebtorrent.com",
            "wss://tracker.webtorrent.dev",
        };
    }

    std::vector<std::string> Lobby::default_ice_servers()
    {
        return {
            "stun:stun.l.google.com:19302",
            "stun:stun1.l.google.com:19302",
            "stun:stun.cloudflare.com:3478",
        };
    }

    struct Lobby::Impl
    {
        LobbyConfig cfg;
        std::mt19937 rng{ std::random_device{}() };
        std::string self_id;
        std::string info_hash;

        mutable std::recursive_mutex mutex;
        std::deque<Event> events;
        std::atomic<bool> running{ false };

        struct Peer
        {
            std::string id;
            std::shared_ptr<rtc::PeerConnection> pc;
            std::shared_ptr<rtc::DataChannel> control; // reliable, ordered JSON messages
            std::shared_ptr<rtc::DataChannel> match;   // unreliable, unordered rollback traffic
            PeerInfo info;
            bool announced = false;                    // PeerJoined was emitted
            Clock::time_point last_ping{};
            bool challenged_by_us = false;
            bool challenged_us = false;
        };
        std::map<std::string, std::shared_ptr<Peer>> peers; // connected or connecting, by peer id
        // Peers are never destroyed inside libdatachannel callbacks (that can deadlock); they
        // wait here until the worker loop releases them.
        std::vector<std::shared_ptr<Peer>> graveyard;

        struct PendingOffer
        {
            std::shared_ptr<rtc::PeerConnection> pc;
            std::shared_ptr<rtc::DataChannel> control;
            std::shared_ptr<rtc::DataChannel> match;
            Clock::time_point created;
        };
        std::map<std::string, PendingOffer> offers; // our outstanding offers, by offer id

        struct Tracker
        {
            std::string url;
            std::shared_ptr<rtc::WebSocket> ws;
            bool open = false;
            Clock::time_point next_announce{};
            Clock::time_point retry_at{};
        };
        std::vector<std::shared_ptr<Tracker>> trackers;

        PeerStatus status = PeerStatus::Idle;
        std::thread worker;

        explicit Impl(LobbyConfig c) : cfg(std::move(c))
        {
            if (cfg.trackers.empty())
                cfg.trackers = Lobby::default_trackers();
            if (cfg.ice_servers.empty())
                cfg.ice_servers = Lobby::default_ice_servers();
            self_id = random_id(rng);
            info_hash = sha1_hex("ttt2-native-online/" + cfg.build_id + "/" + cfg.room).substr(0, 20);
        }

        void push(Event e)
        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(e));
        }

        // -------------------------------------------------------------- trackers

        void connect_tracker(const std::shared_ptr<Tracker>& t)
        {
            t->ws = std::make_shared<rtc::WebSocket>();
            std::weak_ptr<Tracker> weak = t;
            t->ws->onOpen([this, weak] {
                if (auto t = weak.lock())
                {
                    std::lock_guard lock(mutex);
                    t->open = true;
                    t->next_announce = Clock::now();
                    push({ EventType::TrackerStatus, {}, 0, "connected to " + t->url });
                }
            });
            t->ws->onClosed([this, weak] {
                if (auto t = weak.lock())
                {
                    std::lock_guard lock(mutex);
                    t->open = false;
                    t->retry_at = Clock::now() + std::chrono::seconds(15);
                }
            });
            t->ws->onError([this, weak](std::string error) {
                if (auto t = weak.lock())
                    push({ EventType::TrackerStatus, {}, 0, t->url + ": " + error });
            });
            t->ws->onMessage([this, weak](rtc::message_variant m) {
                auto t = weak.lock();
                if (!t)
                    return;
                if (auto* text = std::get_if<std::string>(&m))
                    on_tracker_message(t, *text);
            });
            try
            {
                t->ws->open(t->url);
            }
            catch (const std::exception& e)
            {
                push({ EventType::TrackerStatus, {}, 0, t->url + ": " + e.what() });
                t->retry_at = Clock::now() + std::chrono::seconds(30);
            }
        }

        // Creates a peer connection with both channels and calls `ready` with the complete
        // local description once ICE gathering is done (trackers do not trickle candidates).
        std::shared_ptr<rtc::PeerConnection> make_pc(std::function<void(std::string)> ready)
        {
            auto pc = std::make_shared<rtc::PeerConnection>(rtc_config(cfg.ice_servers));
            std::weak_ptr<rtc::PeerConnection> weak = pc;
            pc->onGatheringStateChange([weak, ready](rtc::PeerConnection::GatheringState s) {
                if (s != rtc::PeerConnection::GatheringState::Complete)
                    return;
                if (auto pc = weak.lock())
                    if (auto d = pc->localDescription())
                        ready(std::string(*d));
            });
            return pc;
        }

        void announce(const std::shared_ptr<Tracker>& t)
        {
            // Fresh offers each announce; old unanswered ones expire.
            const auto now = Clock::now();
            for (auto it = offers.begin(); it != offers.end();)
                it = now - it->second.created > kOfferLifetime ? offers.erase(it) : std::next(it);

            if (int(peers.size()) >= cfg.max_peers)
            {
                send_json(t, { { "action", "announce" }, { "info_hash", info_hash }, { "peer_id", self_id }, { "numwant", 0 },
                               { "uploaded", 0 }, { "downloaded", 0 }, { "offers", json::array() } });
                return;
            }

            auto collected = std::make_shared<std::vector<json>>();
            auto remaining = std::make_shared<std::atomic<int>>(kOffersPerAnnounce);
            std::weak_ptr<Tracker> weak = t;
            for (int i = 0; i < kOffersPerAnnounce; i++)
            {
                const std::string offer_id = random_id(rng);
                PendingOffer po;
                po.created = now;
                po.pc = make_pc([this, weak, offer_id, collected, remaining](std::string sdp) {
                    std::lock_guard lock(mutex);
                    collected->push_back({ { "offer_id", offer_id }, { "offer", { { "type", "offer" }, { "sdp", sdp } } } });
                    if (--*remaining == 0)
                    {
                        if (auto t = weak.lock(); t && t->open)
                            send_json(t, { { "action", "announce" }, { "info_hash", info_hash }, { "peer_id", self_id },
                                           { "numwant", kOffersPerAnnounce }, { "uploaded", 0 }, { "downloaded", 0 },
                                           { "event", "started" }, { "offers", *collected } });
                    }
                });
                po.control = po.pc->createDataChannel("control");
                rtc::DataChannelInit unreliable;
                unreliable.reliability.unordered = true;
                unreliable.reliability.maxRetransmits = 0;
                po.match = po.pc->createDataChannel("match", unreliable);
                offers[offer_id] = std::move(po);
            }
        }

        void send_json(const std::shared_ptr<Tracker>& t, const json& j)
        {
            try
            {
                t->ws->send(j.dump());
            }
            catch (...)
            {
            }
        }

        void on_tracker_message(const std::shared_ptr<Tracker>& t, const std::string& text)
        {
            json j = json::parse(text, nullptr, false);
            if (j.is_discarded() || j.value("info_hash", "") != info_hash)
                return;

            if (j.contains("interval"))
            {
                std::lock_guard lock(mutex);
                const int interval = std::clamp(j.value("interval", 30), 10, 120);
                t->next_announce = Clock::now() + std::chrono::seconds(interval);
            }

            const std::string from = j.value("peer_id", "");
            if (from.empty() || from == self_id)
                return;

            if (j.contains("offer") && j["offer"].is_object())
                on_offer(t, from, j.value("offer_id", ""), j["offer"].value("sdp", ""));
            else if (j.contains("answer") && j["answer"].is_object())
                on_answer(from, j.value("offer_id", ""), j["answer"].value("sdp", ""));
        }

        void on_offer(const std::shared_ptr<Tracker>& t, const std::string& from, const std::string& offer_id, const std::string& sdp)
        {
            std::lock_guard lock(mutex);
            // Both sides may connect to each other at once; the lower id's connection wins.
            if (peers.count(from) || int(peers.size()) >= cfg.max_peers || sdp.empty())
                return;

            auto peer = std::make_shared<Peer>();
            peer->id = from;
            std::weak_ptr<Tracker> weak = t;
            peer->pc = make_pc([this, weak, from, offer_id](std::string answer) {
                if (auto t = weak.lock(); t && t->open)
                    send_json(t, { { "action", "announce" }, { "info_hash", info_hash }, { "peer_id", self_id }, { "to_peer_id", from },
                                   { "offer_id", offer_id }, { "answer", { { "type", "answer" }, { "sdp", answer } } } });
            });
            std::weak_ptr<Peer> wp = peer;
            peer->pc->onDataChannel([this, wp](std::shared_ptr<rtc::DataChannel> dc) {
                if (auto p = wp.lock())
                {
                    std::lock_guard lock(mutex);
                    if (dc->label() == "control")
                        attach_control(p, dc);
                    else if (dc->label() == "match")
                        p->match = dc;
                }
            });
            watch_state(peer);
            peers[from] = peer;
            try
            {
                peer->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
            }
            catch (...)
            {
                peers.erase(from);
            }
        }

        void on_answer(const std::string& from, const std::string& offer_id, const std::string& sdp)
        {
            std::lock_guard lock(mutex);
            auto it = offers.find(offer_id);
            if (it == offers.end() || sdp.empty())
                return;
            if (auto existing = peers.find(from); existing != peers.end())
            {
                // Crossed connections: keep the one created by the peer with the lower id.
                if (self_id > from)
                    return;
                graveyard.push_back(existing->second);
                peers.erase(existing);
            }

            auto peer = std::make_shared<Peer>();
            peer->id = from;
            peer->pc = it->second.pc;
            peer->match = it->second.match;
            attach_control(peer, it->second.control);
            offers.erase(it);
            watch_state(peer);
            peers[from] = peer;
            try
            {
                peer->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
            }
            catch (...)
            {
                peers.erase(from);
            }
        }

        void watch_state(const std::shared_ptr<Peer>& peer)
        {
            const std::string id = peer->id;
            peer->pc->onStateChange([this, id](rtc::PeerConnection::State s) {
                if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed ||
                    s == rtc::PeerConnection::State::Disconnected)
                    drop_peer(id);
            });
        }

        void drop_peer(const std::string& id)
        {
            std::lock_guard lock(mutex);
            auto it = peers.find(id);
            if (it == peers.end())
                return;
            const bool was_announced = it->second->announced;
            graveyard.push_back(it->second);
            peers.erase(it);
            if (was_announced)
                push({ EventType::PeerLeft, id, 0, {} });
        }

        // -------------------------------------------------------------- control channel

        void attach_control(const std::shared_ptr<Peer>& peer, std::shared_ptr<rtc::DataChannel> dc)
        {
            peer->control = dc;
            std::weak_ptr<Peer> wp = peer;
            dc->onOpen([this, wp] {
                if (auto p = wp.lock())
                    send_hello(p);
            });
            dc->onMessage([this, wp](rtc::message_variant m) {
                auto p = wp.lock();
                if (!p)
                    return;
                if (auto* text = std::get_if<std::string>(&m))
                    on_control(p, *text);
            });
            if (dc->isOpen())
                send_hello(peer);
        }

        void send_control(const std::shared_ptr<Peer>& p, const json& j)
        {
            if (p->control && p->control->isOpen())
            {
                try
                {
                    p->control->send(j.dump());
                }
                catch (...)
                {
                }
            }
        }

        static const char* status_name(PeerStatus s)
        {
            switch (s)
            {
            case PeerStatus::InMatch: return "match";
            case PeerStatus::Away: return "away";
            default: return "idle";
            }
        }

        void send_hello(const std::shared_ptr<Peer>& p)
        {
            json hello = { { "t", "hello" }, { "name", cfg.display_name.substr(0, 24) }, { "status", status_name(status) },
                           { "build", cfg.build_id } };
            if (!cfg.avatar_png.empty() && cfg.avatar_png.size() <= kMaxAvatarBytes)
                hello["avatar"] = base64_encode(cfg.avatar_png);
            send_control(p, hello);
        }

        void on_control(const std::shared_ptr<Peer>& p, const std::string& text)
        {
            json j = json::parse(text, nullptr, false);
            if (j.is_discarded())
                return;
            const std::string type = j.value("t", "");
            std::lock_guard lock(mutex);

            if (type == "hello" || type == "status")
            {
                if (type == "hello" && j.value("build", "") != cfg.build_id)
                {
                    drop_peer(p->id); // cannot happen with the room hash, but never pair mismatched builds
                    return;
                }
                if (j.contains("name"))
                    p->info.name = j.value("name", "Player").substr(0, 24);
                if (j.contains("avatar") && j["avatar"].is_string() && j["avatar"].get_ref<const std::string&>().size() <= kMaxAvatarBytes * 4 / 3 + 4)
                    p->info.avatar_png = base64_decode(j["avatar"].get<std::string>());
                const std::string st = j.value("status", "idle");
                p->info.status = st == "match" ? PeerStatus::InMatch : st == "away" ? PeerStatus::Away : PeerStatus::Idle;
                p->info.id = p->id;
                if (!p->announced)
                {
                    p->announced = true;
                    push({ EventType::PeerJoined, p->id, 0, {} });
                }
                else
                {
                    push({ EventType::PeerUpdated, p->id, 0, {} });
                }
            }
            else if (type == "ping")
            {
                send_control(p, { { "t", "pong" }, { "ts", j.value("ts", int64_t(0)) } });
            }
            else if (type == "pong")
            {
                const int64_t sent = j.value("ts", int64_t(0));
                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                if (sent > 0 && now_ms >= sent)
                {
                    const int sample = int(now_ms - sent);
                    p->info.ping_ms = p->info.ping_ms < 0 ? sample : (p->info.ping_ms * 3 + sample) / 4;
                    push({ EventType::PeerUpdated, p->id, 0, {} });
                }
            }
            else if (type == "challenge")
            {
                if (status != PeerStatus::Idle)
                {
                    send_control(p, { { "t", "decline" } });
                    return;
                }
                p->challenged_us = true;
                push({ EventType::ChallengeReceived, p->id, 0, {} });
            }
            else if (type == "accept" && p->challenged_by_us)
            {
                p->challenged_by_us = false;
                // The challenger plays on the left (game port 0).
                push({ EventType::MatchReady, p->id, 0, {} });
            }
            else if (type == "decline")
            {
                p->challenged_by_us = false;
                push({ EventType::ChallengeDeclined, p->id, 0, {} });
            }
        }

        // -------------------------------------------------------------- loop

        void run()
        {
            for (const auto& url : cfg.trackers)
            {
                auto t = std::make_shared<Tracker>();
                t->url = url;
                trackers.push_back(t);
                connect_tracker(t);
            }

            while (running)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const auto now = Clock::now();
                std::lock_guard lock(mutex);
                graveyard.clear();

                for (auto& t : trackers)
                {
                    if (t->open && now >= t->next_announce)
                    {
                        t->next_announce = now + kAnnounceInterval;
                        announce(t);
                    }
                    else if (!t->open && t->retry_at != Clock::time_point{} && now >= t->retry_at)
                    {
                        t->retry_at = {};
                        connect_tracker(t);
                    }
                }

                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                for (auto& [id, p] : peers)
                {
                    if (p->control && p->control->isOpen() && now - p->last_ping > kPingInterval)
                    {
                        p->last_ping = now;
                        send_control(p, { { "t", "ping" }, { "ts", now_ms } });
                    }
                }
            }

            // libdatachannel may still deliver callbacks while connections close; detach them
            // all first so none can reach this object after it is destroyed.
            std::lock_guard lock(mutex);
            auto quiet_pc = [](const std::shared_ptr<rtc::PeerConnection>& pc) {
                if (!pc)
                    return;
                pc->onStateChange(nullptr);
                pc->onGatheringStateChange(nullptr);
                pc->onDataChannel(nullptr);
                pc->close();
            };
            auto quiet_dc = [](const std::shared_ptr<rtc::DataChannel>& dc) {
                if (!dc)
                    return;
                dc->onOpen(nullptr);
                dc->onMessage(nullptr);
            };
            for (auto& t : trackers)
                if (t->ws)
                {
                    t->ws->onOpen(nullptr);
                    t->ws->onClosed(nullptr);
                    t->ws->onError(nullptr);
                    t->ws->onMessage(nullptr);
                    t->ws->close();
                }
            for (auto& [id, o] : offers)
            {
                quiet_dc(o.control);
                quiet_pc(o.pc);
            }
            for (auto& [id, p] : peers)
            {
                quiet_dc(p->control);
                quiet_pc(p->pc);
            }
            for (auto& p : graveyard)
            {
                quiet_dc(p->control);
                quiet_pc(p->pc);
            }
            trackers.clear();
            offers.clear();
            peers.clear();
            graveyard.clear();
        }
    };

    Lobby::Lobby(LobbyConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

    Lobby::~Lobby() { stop(); }

    void Lobby::start()
    {
        if (impl_->running.exchange(true))
            return;
        impl_->worker = std::thread([this] { impl_->run(); });
    }

    void Lobby::stop()
    {
        if (!impl_->running.exchange(false))
            return;
        if (impl_->worker.joinable())
            impl_->worker.join();
    }

    std::vector<PeerInfo> Lobby::peers() const
    {
        std::lock_guard lock(impl_->mutex);
        std::vector<PeerInfo> out;
        for (auto& [id, p] : impl_->peers)
            if (p->announced)
                out.push_back(p->info);
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
        return out;
    }

    std::vector<Event> Lobby::poll_events()
    {
        std::lock_guard lock(impl_->mutex);
        std::vector<Event> out(impl_->events.begin(), impl_->events.end());
        impl_->events.clear();
        return out;
    }

    void Lobby::set_status(PeerStatus status)
    {
        std::lock_guard lock(impl_->mutex);
        impl_->status = status;
        for (auto& [id, p] : impl_->peers)
            impl_->send_control(p, { { "t", "status" }, { "status", Impl::status_name(status) } });
    }

    void Lobby::challenge(const std::string& peer_id)
    {
        std::lock_guard lock(impl_->mutex);
        if (auto it = impl_->peers.find(peer_id); it != impl_->peers.end())
        {
            it->second->challenged_by_us = true;
            impl_->send_control(it->second, { { "t", "challenge" } });
        }
    }

    void Lobby::accept(const std::string& peer_id)
    {
        std::lock_guard lock(impl_->mutex);
        if (auto it = impl_->peers.find(peer_id); it != impl_->peers.end() && it->second->challenged_us)
        {
            it->second->challenged_us = false;
            impl_->send_control(it->second, { { "t", "accept" } });
            impl_->push({ EventType::MatchReady, peer_id, 1, {} });
        }
    }

    void Lobby::decline(const std::string& peer_id)
    {
        std::lock_guard lock(impl_->mutex);
        if (auto it = impl_->peers.find(peer_id); it != impl_->peers.end())
        {
            it->second->challenged_us = false;
            impl_->send_control(it->second, { { "t", "decline" } });
        }
    }

    std::unique_ptr<netplay::Transport> Lobby::take_match_transport(const std::string& peer_id)
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->peers.find(peer_id);
        if (it == impl_->peers.end() || !it->second->match)
            return nullptr;
        return std::make_unique<ChannelTransport>(it->second->match);
    }

    // ------------------------------------------------------------------ relay

    LoopbackRelay::LoopbackRelay(std::unique_ptr<netplay::Transport> remote, uint16_t relay_port, uint16_t game_port, std::string* error)
        : remote_(std::move(remote)), local_(netplay::make_udp_transport(relay_port, "127.0.0.1", game_port, error))
    {
    }

    LoopbackRelay::~LoopbackRelay() = default;

    bool LoopbackRelay::ok() const { return remote_ && local_; }

    void LoopbackRelay::pump()
    {
        if (!ok())
            return;
        std::vector<uint8_t> packet;
        while (local_->receive(packet))
            remote_->send(packet.data(), packet.size());
        while (remote_->receive(packet))
            local_->send(packet.data(), packet.size());
    }
}
