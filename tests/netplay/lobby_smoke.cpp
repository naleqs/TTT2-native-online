// Lobby smoke test (needs internet; not run by ctest): two lobbies in one process join a random
// private room through the public trackers, find each other, go through challenge/accept and
// exchange datagrams over the WebRTC match channel.

#include <tt2/netplay/lobby.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>

using namespace tt2::lobby;
using Clock = std::chrono::steady_clock;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(std::random_device{}());
    const std::string room = "smoke-" + std::to_string(rng());

    auto make = [&](const char* name) {
        LobbyConfig c;
        c.build_id = "smoke-test";
        c.display_name = name;
        c.room = room;
        return std::make_unique<Lobby>(c);
    };
    auto a = make("Alice");
    auto b = make("Bob");
    a->start();
    b->start();
    std::printf("room %s: waiting for the peers to find each other...\n", room.c_str());

    const auto deadline = Clock::now() + std::chrono::seconds(90);
    std::string b_id;
    while (Clock::now() < deadline)
    {
        for (auto& e : a->poll_events())
            if (e.type == EventType::TrackerStatus)
                std::printf("  A: %s\n", e.message.c_str());
        b->poll_events();
        const auto peers = a->peers();
        if (!peers.empty() && !b->peers().empty())
        {
            b_id = peers[0].id;
            std::printf("A sees '%s' (%d ms), B sees '%s'\n", peers[0].name.c_str(), peers[0].ping_ms, b->peers()[0].name.c_str());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (b_id.empty())
    {
        std::printf("FAIL: peers did not find each other\n");
        return 1;
    }

    a->challenge(b_id);
    std::string a_id;
    bool a_ready = false, b_ready = false;
    while (Clock::now() < deadline && !(a_ready && b_ready))
    {
        for (auto& e : b->poll_events())
        {
            if (e.type == EventType::ChallengeReceived)
            {
                a_id = e.peer_id;
                b->accept(e.peer_id);
            }
            if (e.type == EventType::MatchReady)
                b_ready = true;
        }
        for (auto& e : a->poll_events())
            if (e.type == EventType::MatchReady)
                a_ready = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!a_ready || !b_ready)
    {
        std::printf("FAIL: challenge was not accepted\n");
        return 1;
    }
    std::printf("match ready\n");

    auto ta = a->take_match_transport(b_id);
    auto tb = b->take_match_transport(a_id);
    if (!ta || !tb)
    {
        std::printf("FAIL: no match channel\n");
        return 1;
    }
    int received = 0;
    std::vector<uint8_t> packet;
    for (int i = 0; i < 200 && Clock::now() < deadline; i++)
    {
        const uint8_t msg[4] = { 'p', 'i', 'n', uint8_t(i) };
        ta->send(msg, sizeof(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        while (tb->receive(packet))
            received++;
    }
    std::printf("match channel: %d of 200 datagrams arrived\n", received);
    a.reset();
    b.reset();
    if (received < 150)
    {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("passed\n");
    return 0;
}
