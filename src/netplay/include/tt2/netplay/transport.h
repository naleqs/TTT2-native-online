#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tt2::netplay
{
    // Unreliable, unordered datagram pipe to exactly one peer. The rollback protocol does its
    // own redundancy (every input packet repeats all unacknowledged inputs), so the
    // transport must never block or retransmit.
    class Transport
    {
    public:
        virtual ~Transport() = default;

        virtual void send(const uint8_t* data, size_t size) = 0;

        // Non-blocking. Returns false when no datagram is waiting.
        virtual bool receive(std::vector<uint8_t>& out) = 0;

        // False once the underlying connection is known to be gone (WebRTC channel closed).
        // A UDP transport cannot know this and always returns true; the session's own
        // timeout handles silence.
        virtual bool open() const { return true; }
    };

    // Plain UDP to a fixed address (LAN, port-forwarded or VPN play).
    std::unique_ptr<Transport> make_udp_transport(uint16_t local_port, const std::string& remote_host, uint16_t remote_port, std::string* error);
}
