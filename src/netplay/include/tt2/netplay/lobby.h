#pragma once

// Serverless online lobby.
//
// There is no lobby server to run or pay for. Players find each other through public
// WebTorrent trackers, which only relay WebRTC connection offers between peers that announce
// the same room hash (the technique the Trystero library uses for browser apps). Once two
// peers have exchanged offers, they talk directly over WebRTC data channels; STUN does NAT
// hole punching, and an optional TURN server (configured in .env) covers strict NATs.
//
// Only peers with the same build id meet: the room hash includes it, so players never see
// someone whose game would desync with theirs.

#include <tt2/netplay/transport.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tt2::lobby
{
    struct LobbyConfig
    {
        std::string build_id;                 // from native/bin/build-info.json
        std::string display_name = "Player";
        std::string avatar_png;               // small PNG (profile picture), sent to other players; may be empty
        std::string room = "public";          // private rooms: any shared word
        std::vector<std::string> trackers;    // wss:// WebTorrent trackers
        std::vector<std::string> ice_servers; // stun:host:port, turn:user:pass@host:port
        int max_peers = 24;
    };

    enum class PeerStatus
    {
        Idle,
        InMatch,
        Away,
    };

    struct PeerInfo
    {
        std::string id;
        std::string name;
        PeerStatus status = PeerStatus::Idle;
        int ping_ms = -1;
        std::string avatar_png;               // their profile picture (PNG bytes), empty if none
    };

    enum class EventType
    {
        PeerJoined,
        PeerLeft,
        PeerUpdated,
        ChallengeReceived,   // peer_id challenged us: accept() or decline()
        ChallengeDeclined,
        MatchReady,          // both sides agreed; take_match_transport(peer_id), local_player says our side
        TrackerStatus,       // message: connected/failed tracker (informational)
    };

    struct Event
    {
        EventType type{};
        std::string peer_id;
        int local_player = 0;
        std::string message;
    };

    class Lobby
    {
    public:
        explicit Lobby(LobbyConfig config);
        ~Lobby();

        Lobby(const Lobby&) = delete;
        Lobby& operator=(const Lobby&) = delete;

        void start();
        void stop();

        std::vector<PeerInfo> peers() const;
        std::vector<Event> poll_events();

        void set_status(PeerStatus status);
        void challenge(const std::string& peer_id);
        void accept(const std::string& peer_id);
        void decline(const std::string& peer_id);

        // After MatchReady: an unreliable, unordered datagram channel to that peer for the
        // rollback session. The launcher relays it to the game process over loopback UDP.
        std::unique_ptr<netplay::Transport> take_match_transport(const std::string& peer_id);

        static std::vector<std::string> default_trackers();
        static std::vector<std::string> default_ice_servers();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Forwards datagrams between a Transport (the WebRTC match channel) and the game process
    // on 127.0.0.1. pump() is called from the launcher's loop while the game runs.
    class LoopbackRelay
    {
    public:
        LoopbackRelay(std::unique_ptr<netplay::Transport> remote, uint16_t relay_port, uint16_t game_port, std::string* error);
        ~LoopbackRelay();
        bool ok() const;
        void pump();

    private:
        std::unique_ptr<netplay::Transport> remote_;
        std::unique_ptr<netplay::Transport> local_;
    };
}
