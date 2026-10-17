// System configuration, modules, crypto, debug and the (offline) network stack.

#include "objects.h"
#include "../memory.h"

using namespace tt2;
using namespace tt2::kernel;

namespace
{
    // SHA-1 for XeCryptSha (saves may be checksummed with it).
    struct Sha1
    {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        uint8_t block[64];
        size_t used = 0;
        uint64_t total = 0;

        static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

        void compress()
        {
            uint32_t w[80];
            for (int i = 0; i < 16; i++)
                w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
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

        void update(const uint8_t* p, size_t n)
        {
            total += n;
            while (n--)
            {
                block[used++] = *p++;
                if (used == 64)
                {
                    compress();
                    used = 0;
                }
            }
        }

        void finish(uint8_t out[20])
        {
            const uint64_t bits = total * 8;
            const uint8_t pad = 0x80;
            update(&pad, 1);
            const uint8_t zero = 0;
            while (used != 56)
                update(&zero, 1);
            for (int i = 7; i >= 0; i--)
            {
                const uint8_t b = uint8_t(bits >> (i * 8));
                update(&b, 1);
            }
            for (int i = 0; i < 5; i++)
                for (int j = 0; j < 4; j++)
                    out[i * 4 + j] = uint8_t(h[i] >> (24 - j * 8));
        }
    };

    uint32_t g_module_handle = 0; // fake LDR entry for Default.xex
}

// ---------------------------------------------------------------------- config

// ExGetXConfigSetting(category, setting, buffer, buffer_size, required_size*)
TT2_IMPORT(ExGetXConfigSetting)
{
    Args a{ ctx };
    const uint32_t category = a.u32(0), setting = a.u32(1), buffer = a.u32(2), size = a.u32(3);
    uint32_t value = 0, value_size = 4;
    bool known = true;
    if (category == 0x0003) // user
    {
        switch (setting)
        {
        case 0x0001: value = 0; break;            // time zone bias
        case 0x0007: value = 0x00040000; break;   // video flags: widescreen
        case 0x0009: value = 1; break;            // language: English
        case 0x000A: value = 0x00010001; break;   // audio flags: stereo
        case 0x000C: value = 0; break;            // retail flags
        case 0x000E: value = 103; break;          // country: United States
        default: value = 0; break;
        }
    }
    else if (category == 0x0002 && setting == 0x0002) // secured: AV region
        value = 0x00001000;                          // NTSC-M
    else
        known = false;

    if (!known)
        log("ExGetXConfigSetting(%04X, %04X) -> 0", category, setting);
    if (buffer && size)
    {
        std::memset(g_base + buffer, 0, size);
        if (size >= 4)
            store32(buffer, value);
        else if (size == 2)
            store16(buffer, uint16_t(value));
        else
            g_base[buffer] = uint8_t(value);
    }
    if (auto* r = a.ptr<be<uint16_t>>(4))
        *r = uint16_t(value_size);
    a.ret(status::success);
}

TT2_IMPORT(ExRegisterTitleTerminateNotification) {}

// ---------------------------------------------------------------------- modules

TT2_IMPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

// XexGetModuleHandle(name, handle*)
TT2_IMPORT(XexGetModuleHandle)
{
    Args a{ ctx };
    const char* name = a.u32(0) ? reinterpret_cast<const char*>(g_base + a.u32(0)) : nullptr;
    if (!g_module_handle)
        g_module_handle = g_memory.system_alloc(0x100);
    uint32_t handle = 0;
    if (!name || _stricmp(name, "default.xex") == 0)
        handle = g_module_handle;
    else if (_stricmp(name, "xam.xex") == 0 || _stricmp(name, "xboxkrnl.exe") == 0)
        handle = g_module_handle + 0x80;
    if (auto* h = a.ptr<be<uint32_t>>(1))
        *h = handle;
    a.ret(handle ? status::success : 0xC0000135); // STATUS_DLL_NOT_FOUND
}

TT2_IMPORT(XexGetProcedureAddress)
{
    Args a{ ctx };
    log("XexGetProcedureAddress(%08X, ordinal %u) not available", a.u32(0), a.u32(1));
    if (auto* p = a.ptr<be<uint32_t>>(2))
        *p = 0;
    a.ret(0xC0000139); // STATUS_ENTRYPOINT_NOT_FOUND
}

TT2_IMPORT(XexGetModuleSection)
{
    Args a{ ctx };
    log("XexGetModuleSection('%s') not available", a.u32(1) ? reinterpret_cast<const char*>(g_base + a.u32(1)) : "");
    a.ret(0xC0000225); // STATUS_NOT_FOUND
}

TT2_IMPORT(XexLoadImage) { ctx.r3.u64 = status::not_supported; }
TT2_IMPORT(XexUnloadImage) { ctx.r3.u64 = status::success; }
TT2_IMPORT(XexLoadImageHeaders) { ctx.r3.u64 = status::not_supported; }

// ---------------------------------------------------------------------- crypto

// XeCryptSha(in1, len1, in2, len2, in3, len3, out, out_len)
TT2_IMPORT(XeCryptSha)
{
    Args a{ ctx };
    Sha1 sha;
    for (int i = 0; i < 3; i++)
        if (a.u32(i * 2) && a.u32(i * 2 + 1))
            sha.update(g_base + a.u32(i * 2), a.u32(i * 2 + 1));
    uint8_t digest[20];
    sha.finish(digest);
    if (a.u32(6))
        std::memcpy(g_base + a.u32(6), digest, std::min<uint32_t>(20, a.u32(7)));
}

TT2_IMPORT(XeCryptRotSumSha)
{
    Args a{ ctx };
    // Only used to sign console-bound data; any stable digest works offline.
    Sha1 sha;
    for (int i = 0; i < 2; i++)
        if (a.u32(i * 2) && a.u32(i * 2 + 1))
            sha.update(g_base + a.u32(i * 2), a.u32(i * 2 + 1));
    uint8_t digest[20];
    sha.finish(digest);
    if (a.u32(4))
        std::memcpy(g_base + a.u32(4), digest, std::min<uint32_t>(20, a.u32(5)));
}

TT2_IMPORT(XeCryptBnQwBeSigVerify) { ctx.r3.u64 = 1; }
TT2_IMPORT(XeKeysConsolePrivateKeySign)
{
    Args a{ ctx };
    if (a.u32(1))
        std::memset(g_base + a.u32(1), 0, 0x228);
    a.ret(1);
}
TT2_IMPORT(XeKeysConsoleSignatureVerification) { ctx.r3.u64 = 1; }
TT2_IMPORT(XeKeysGetKey) { ctx.r3.u64 = status::unsuccessful; }

// ---------------------------------------------------------------------- debug / fatal

TT2_IMPORT(DbgBreakPoint) { log("guest: DbgBreakPoint"); }

TT2_IMPORT(KeBugCheck) { fatal("guest KeBugCheck(%08X)", ctx.r3.u32); }
TT2_IMPORT(KeBugCheckEx) { fatal("guest KeBugCheckEx(%08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }

TT2_IMPORT(HalReturnToFirmware)
{
    log("game returned to the dashboard");
    std::exit(0);
}

TT2_IMPORT(KeLockL2) { ctx.r3.u64 = 0; }
TT2_IMPORT(KeUnlockL2) {}
TT2_IMPORT(KiApcNormalRoutineNop) {}

// ---------------------------------------------------------------------- I/O manager / STFS

TT2_IMPORT(IoCreateDevice) { ctx.r3.u64 = status::not_supported; }
TT2_IMPORT(IoDeleteDevice) {}
TT2_IMPORT(IoCompleteRequest) {}
TT2_IMPORT(IoInvalidDeviceRequest) { ctx.r3.u64 = status::invalid_parameter; }
TT2_IMPORT(IoCheckShareAccess) { ctx.r3.u64 = status::success; }
TT2_IMPORT(IoSetShareAccess) {}
TT2_IMPORT(IoRemoveShareAccess) {}
TT2_IMPORT(StfsCreateDevice) { ctx.r3.u64 = status::not_supported; }
TT2_IMPORT(StfsControlDevice) { ctx.r3.u64 = status::not_supported; }

// ---------------------------------------------------------------------- network (offline)
// The original online mode needs Xbox Live; TTT2 Native Online uses its own rollback netplay
// instead. The stack reports "no cable" so the game stays in offline mode.

namespace
{
    constexpr uint32_t WSAENETDOWN = 10050;
    thread_local uint32_t t_wsa_error = 0;
    uint32_t fail(uint32_t error = WSAENETDOWN)
    {
        t_wsa_error = error;
        return 0xFFFFFFFF; // SOCKET_ERROR / INVALID_SOCKET
    }
}

TT2_IMPORT(NetDll_WSAStartup)
{
    Args a{ ctx };
    if (const uint32_t data = a.u32(2))
    {
        std::memset(g_base + data, 0, 0x190);
        store16(data, 0x0202);
        store16(data + 2, 0x0202);
    }
    a.ret(0);
}
TT2_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = t_wsa_error; }
TT2_IMPORT(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; } // no link
TT2_IMPORT(NetDll_XNetGetTitleXnAddr)
{
    Args a{ ctx };
    if (a.u32(1))
        std::memset(g_base + a.u32(1), 0, 0x24);
    a.ret(0x00000001); // XNET_GET_XNADDR_NONE
}
TT2_IMPORT(NetDll_XnpLogonGetStatus) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_XNetRandom)
{
    Args a{ ctx };
    for (uint32_t i = 0; i < a.u32(2); i++)
        g_base[a.u32(1) + i] = uint8_t(rand());
    a.ret(0);
}
TT2_IMPORT(NetDll_XNetCreateKey) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetRegisterKey) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetServerToInAddr) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetUnregisterInAddr) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_XNetConnect) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetQosServiceLookup) { ctx.r3.u64 = WSAENETDOWN; }
TT2_IMPORT(NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_socket) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_closesocket) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_shutdown) { ctx.r3.u64 = 0; }
TT2_IMPORT(NetDll_ioctlsocket) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_setsockopt) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_getsockopt) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_getsockname) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_bind) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_connect) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_listen) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_accept) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_select) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_recv) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_recvfrom) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_send) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_sendto) { ctx.r3.u64 = fail(); }
TT2_IMPORT(NetDll_inet_addr) { ctx.r3.u64 = 0xFFFFFFFF; }
TT2_IMPORT(NetDll___WSAFDIsSet) { ctx.r3.u64 = 0; }
