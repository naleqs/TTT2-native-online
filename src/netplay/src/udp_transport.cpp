// Plain UDP transport for direct connections (LAN, port forwarding, VPNs such as Tailscale).

#include <tt2/netplay/transport.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace tt2::netplay
{
    namespace
    {
        struct WinsockInit
        {
            bool ok = false;
            WinsockInit()
            {
                WSADATA data;
                ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }
            ~WinsockInit()
            {
                if (ok)
                    WSACleanup();
            }
        };

        bool winsock()
        {
            static WinsockInit init;
            return init.ok;
        }

        class UdpTransport final : public Transport
        {
        public:
            UdpTransport(SOCKET s, sockaddr_storage remote, int remote_len) : sock_(s), remote_(remote), remote_len_(remote_len) {}

            ~UdpTransport() override { closesocket(sock_); }

            void send(const uint8_t* data, size_t size) override
            {
                sendto(sock_, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<const sockaddr*>(&remote_), remote_len_);
            }

            bool receive(std::vector<uint8_t>& out) override
            {
                for (;;)
                {
                    char buf[1500];
                    sockaddr_storage from{};
                    int from_len = sizeof(from);
                    const int n = recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
                    if (n == SOCKET_ERROR)
                    {
                        // WSAECONNRESET: an ICMP "port unreachable" for an earlier send; the peer
                        // is not up yet. Keep reading.
                        if (WSAGetLastError() == WSAECONNRESET)
                            continue;
                        return false;
                    }
                    if (!same_peer(from, from_len))
                        continue; // only the configured peer may talk to the session
                    out.assign(buf, buf + n);
                    return true;
                }
            }

        private:
            bool same_peer(const sockaddr_storage& from, int len) const
            {
                if (from.ss_family != remote_.ss_family || len != remote_len_)
                    return false;
                if (from.ss_family == AF_INET)
                {
                    auto& a = reinterpret_cast<const sockaddr_in&>(from);
                    auto& b = reinterpret_cast<const sockaddr_in&>(remote_);
                    return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
                }
                auto& a = reinterpret_cast<const sockaddr_in6&>(from);
                auto& b = reinterpret_cast<const sockaddr_in6&>(remote_);
                return a.sin6_port == b.sin6_port && std::memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0;
            }

            SOCKET sock_;
            sockaddr_storage remote_;
            int remote_len_;
        };
    }

    std::unique_ptr<Transport> make_udp_transport(uint16_t local_port, const std::string& remote_host, uint16_t remote_port, std::string* error)
    {
        auto fail = [&](const std::string& what) -> std::unique_ptr<Transport> {
            if (error)
                *error = what + " (error " + std::to_string(WSAGetLastError()) + ")";
            return nullptr;
        };

        if (!winsock())
            return fail("Winsock unavailable");

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(remote_port);
        if (getaddrinfo(remote_host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
            return fail("cannot resolve " + remote_host);

        sockaddr_storage remote{};
        const int remote_len = int(res->ai_addrlen);
        std::memcpy(&remote, res->ai_addr, res->ai_addrlen);
        const int family = res->ai_family;
        freeaddrinfo(res);

        SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return fail("socket");

        sockaddr_storage local{};
        int local_len;
        if (family == AF_INET6)
        {
            auto& a = reinterpret_cast<sockaddr_in6&>(local);
            a.sin6_family = AF_INET6;
            a.sin6_port = htons(local_port);
            a.sin6_addr = in6addr_any;
            local_len = sizeof(sockaddr_in6);
        }
        else
        {
            auto& a = reinterpret_cast<sockaddr_in&>(local);
            a.sin_family = AF_INET;
            a.sin_port = htons(local_port);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            local_len = sizeof(sockaddr_in);
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&local), local_len) == SOCKET_ERROR)
        {
            closesocket(s);
            return fail("cannot bind UDP port " + std::to_string(local_port));
        }

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        return std::make_unique<UdpTransport>(s, remote, remote_len);
    }
}
