// XAM: controllers, profiles, save content, system UI, notifications, tasks, video mode.
// There is no Xbox Live: online features report "offline", and netplay replaces them.

#include "objects.h"
#include "threads.h"
#include "io.h"
#include "../memory.h"
#include "../netplay_glue.h"
#include "../input/input.h"

#include <deque>
#include <filesystem>

namespace fs = std::filesystem;
using namespace tt2;
using namespace tt2::kernel;

namespace tt2::xam
{
    fs::path g_save_root; // set by main: native\save
}

namespace
{
    constexpr uint32_t kTitleId = 0x54656B6B;
    constexpr uint32_t ERROR_EMPTY = 4306;
    constexpr uint32_t ERROR_NOT_FOUND = 1168;
    constexpr uint64_t kXuidBase = 0xE000000054543200ull; // offline XUIDs

    // XOVERLAPPED: +0 result, +4 length, +8 context, +C event, +10 completion routine,
    // +14 completion context, +18 extended error
    uint32_t complete(uint32_t overlapped, uint32_t result, uint32_t length = 0)
    {
        if (!overlapped)
            return result;
        store32(overlapped + 0x18, result);
        store32(overlapped + 4, length);
        store32(overlapped + 0, result);
        if (auto e = std::dynamic_pointer_cast<Event>(object_from_handle(load32(overlapped + 0xC))))
            with_lock([&] {
                e->signaled = true;
                e->mirror_signal_state(1);
            });
        return win32::io_pending;
    }

    bool user_present(uint32_t user)
    {
        // Player 1 is always signed in (a local offline profile); player 2 when a second
        // controller is connected.
        return user == 0 || (user == 1 && input::connected_count() >= 2);
    }

    // ------------------------------------------------------------------ notifications

    struct Listener final : Object
    {
        uint64_t mask;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // id, param
        explicit Listener(uint64_t m) : mask(m) {}
        ObjectType type() const override { return ObjectType::Other; }
        bool is_signaled(uint32_t) const override { return !queue.empty(); }
    };

    // ------------------------------------------------------------------ enumerators

    struct Enumerator final : Object
    {
        std::vector<std::vector<uint8_t>> items;
        size_t position = 0;
        uint32_t item_size = 0;
        ObjectType type() const override { return ObjectType::Other; }
    };

    std::u16string widen(const std::string& s)
    {
        std::u16string w;
        for (char c : s)
            w.push_back(char16_t(uint8_t(c)));
        return w;
    }

    std::string content_filename(uint32_t content_data)
    {
        // XCONTENT_DATA: be32 DeviceID, be32 ContentType, wchar DisplayName[128], char FileName[42]
        const char* name = reinterpret_cast<const char*>(g_base + content_data + 8 + 256);
        return std::string(name, strnlen(name, 42));
    }
}

// ---------------------------------------------------------------------- input

// XINPUT_STATE: be32 PacketNumber, XINPUT_GAMEPAD { be16 Buttons, u8 LT, u8 RT, be16 LX, LY, RX, RY }
TT2_IMPORT(XamInputGetState)
{
    Args a{ ctx };
    const uint32_t user = a.u32(0) == 0xFF ? 0 : a.u32(0);
    const uint32_t state = a.u32(2);
    input::PadState s;
    if (!netglue::input_for_port(int(user), s))
        s = input::read(int(user));
    if (!s.connected)
        return a.ret(win32::device_not_connected);
    if (state)
    {
        static std::atomic<uint32_t> packet{ 1 };
        store32(state, packet++);
        store16(state + 4, s.buttons);
        g_base[state + 6] = s.left_trigger;
        g_base[state + 7] = s.right_trigger;
        store16(state + 8, uint16_t(s.lx));
        store16(state + 10, uint16_t(s.ly));
        store16(state + 12, uint16_t(s.rx));
        store16(state + 14, uint16_t(s.ry));
    }
    a.ret(win32::success);
}

// XINPUT_VIBRATION: be16 left motor, be16 right motor
TT2_IMPORT(XamInputSetState)
{
    Args a{ ctx };
    const uint32_t user = a.u32(0) == 0xFF ? 0 : a.u32(0);
    if (const uint32_t v = a.u32(2))
        input::rumble(int(user), load16(v), load16(v + 2));
    a.ret(input::read(int(user)).connected || netglue::active() ? win32::success : win32::device_not_connected);
}

// XINPUT_CAPABILITIES: u8 Type, u8 SubType, be16 Flags, XINPUT_GAMEPAD, XINPUT_VIBRATION
TT2_IMPORT(XamInputGetCapabilities)
{
    Args a{ ctx };
    const uint32_t user = a.u32(0) == 0xFF ? 0 : a.u32(0);
    input::PadState s;
    if (!netglue::input_for_port(int(user), s))
        s = input::read(int(user));
    if (!s.connected)
        return a.ret(win32::device_not_connected);
    if (const uint32_t caps = a.u32(2))
    {
        std::memset(g_base + caps, 0, 20);
        g_base[caps] = 1;     // XINPUT_DEVTYPE_GAMEPAD
        g_base[caps + 1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        store16(caps + 4, 0xF3FF);
        g_base[caps + 6] = 0xFF;
        g_base[caps + 7] = 0xFF;
        for (int i = 0; i < 4; i++)
            store16(caps + 8 + i * 2, 0xFFC0);
        store16(caps + 16, 0xFFFF);
        store16(caps + 18, 0xFFFF);
    }
    a.ret(win32::success);
}

TT2_IMPORT(XamInputGetKeystrokeEx)
{
    ctx.r3.u64 = ERROR_EMPTY; // controller-only port: no keyboard
}

// ---------------------------------------------------------------------- users

TT2_IMPORT(XamUserGetSigninState)
{
    ctx.r3.u64 = user_present(ctx.r3.u32) ? 1 : 0; // 1 = signed in locally
}

// XUSER_SIGNIN_INFO: be64 xuid, be32 flags, be32 state, be32 guest, be32 sponsor, char name[16]
TT2_IMPORT(XamUserGetSigninInfo)
{
    Args a{ ctx };
    const uint32_t user = a.u32(0), info = a.u32(2);
    if (!info)
        return a.ret(win32::success);
    std::memset(g_base + info, 0, 40);
    if (!user_present(user))
        return a.ret(win32::no_such_user);
    store64(info, kXuidBase + user);
    store32(info + 12, 1);
    const std::string name = user == 0 ? "Player 1" : "Player 2";
    std::memcpy(g_base + info + 24, name.c_str(), name.size());
    a.ret(win32::success);
}

TT2_IMPORT(XamUserGetXUID)
{
    Args a{ ctx };
    if (!user_present(a.u32(0)))
        return a.ret(win32::not_logged_on);
    if (auto* x = a.ptr<be<uint64_t>>(2))
        *x = kXuidBase + a.u32(0);
    a.ret(win32::success);
}

TT2_IMPORT(XamUserGetName)
{
    Args a{ ctx };
    if (!user_present(a.u32(0)))
        return a.ret(win32::no_such_user);
    const std::string name = a.u32(0) == 0 ? "Player 1" : "Player 2";
    const uint32_t len = a.u32(2);
    if (a.u32(1) && len)
    {
        const size_t n = std::min<size_t>(name.size(), len - 1);
        std::memcpy(g_base + a.u32(1), name.data(), n);
        g_base[a.u32(1) + n] = 0;
    }
    a.ret(win32::success);
}

TT2_IMPORT(XamUserCheckPrivilege)
{
    Args a{ ctx };
    if (auto* r = a.ptr<be<uint32_t>>(2))
        *r = 1;
    a.ret(win32::success);
}

TT2_IMPORT(XamUserAreUsersFriends)
{
    Args a{ ctx };
    if (auto* r = a.ptr<be<uint32_t>>(3))
        *r = 0;
    a.ret(complete(a.u32(4), win32::success));
}

TT2_IMPORT(XamUserGetMembershipTierFromXUID) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamUserGetOnlineCountryFromXUID) { ctx.r3.u64 = 103; } // United States

// XamUserReadProfileSettings(title, user, xuid_count, xuids, setting_count, setting_ids,
//                            buffer_size*, buffer, overlapped)
TT2_IMPORT(XamUserReadProfileSettings)
{
    Args a{ ctx };
    // No profile settings are stored: the game falls back to its defaults.
    a.ret(complete(a.u32(8), ERROR_NOT_FOUND));
}

TT2_IMPORT(XamUserGetDeviceContext)
{
    Args a{ ctx };
    if (auto* r = a.ptr<be<uint32_t>>(2))
        *r = 0;
    a.ret(user_present(a.u32(0)) ? win32::success : win32::device_not_connected);
}

TT2_IMPORT(XamUserCreateAchievementEnumerator)
{
    Args a{ ctx };
    auto e = std::make_shared<Enumerator>();
    if (auto* size = a.ptr<be<uint32_t>>(6))
        *size = 0x100;
    if (auto* h = a.ptr<be<uint32_t>>(7))
        *h = create_handle(e);
    a.ret(win32::success);
}

TT2_IMPORT(XamUserCreateStatsEnumerator)
{
    Args a{ ctx };
    auto e = std::make_shared<Enumerator>();
    if (auto* size = a.ptr<be<uint32_t>>(7))
        *size = 0x100;
    if (auto* h = a.ptr<be<uint32_t>>(8))
        *h = create_handle(e);
    a.ret(win32::success);
}

// XamEnumerate(handle, flags, buffer, buffer_size, items_returned*, overlapped)
TT2_IMPORT(XamEnumerate)
{
    Args a{ ctx };
    auto e = handle_as<Enumerator>(a.u32(0));
    const uint32_t buffer = a.u32(2), size = a.u32(3);
    uint32_t returned = 0, result = win32::no_more_files;
    if (e && e->position < e->items.size())
    {
        uint32_t offset = 0;
        while (e->position < e->items.size() && offset + e->items[e->position].size() <= size)
        {
            std::memcpy(g_base + buffer + offset, e->items[e->position].data(), e->items[e->position].size());
            offset += uint32_t(e->items[e->position].size());
            e->position++;
            returned++;
        }
        result = returned ? win32::success : 122; // ERROR_INSUFFICIENT_BUFFER
    }
    if (auto* r = a.ptr<be<uint32_t>>(4))
        *r = returned;
    a.ret(complete(a.u32(5), result, returned));
}

TT2_IMPORT(XamCreateEnumeratorHandle)
{
    Args a{ ctx };
    if (auto* h = a.ptr<be<uint32_t>>(7))
        *h = create_handle(std::make_shared<Enumerator>());
    a.ret(win32::success);
}

TT2_IMPORT(XamGetPrivateEnumStructureFromHandle) { ctx.r3.u64 = status::unsuccessful; }

// ---------------------------------------------------------------------- content (saves)

// XamContentCreateEx(user, root_name, content_data, flags, disposition*, license*,
//                    cache_size, content_size, overlapped)
TT2_IMPORT(XamContentCreateEx)
{
    Args a{ ctx };
    const std::string root = reinterpret_cast<const char*>(g_base + a.u32(1));
    const std::string file = content_filename(a.u32(2));
    const uint32_t flags = a.u32(3);
    const fs::path dir = xam::g_save_root / (file.empty() ? "content" : file);
    std::error_code ec;
    const bool exists = fs::exists(dir, ec);
    // flags: 1 = CREATE_NEW, 2 = CREATE_ALWAYS, 3 = OPEN_EXISTING, 4 = OPEN_ALWAYS, 5 = TRUNCATE_EXISTING
    const uint32_t mode = flags & 0xF;
    if (!exists && (mode == 3 || mode == 5))
        return a.ret(complete(a.u32(8), win32::file_not_found));
    if (exists && mode == 1)
        return a.ret(complete(a.u32(8), 80)); // ERROR_FILE_EXISTS
    fs::create_directories(dir, ec);
    io::mount("\\device\\content\\" + root, dir, false);
    io::link(root + ":", "\\device\\content\\" + root);
    if (auto* d = a.ptr<be<uint32_t>>(4))
        *d = exists ? 2 : 1; // XCONTENT_EXISTING / XCONTENT_NEW
    if (auto* l = a.ptr<be<uint32_t>>(5))
        *l = 0;
    a.ret(complete(a.u32(8), win32::success));
}

TT2_IMPORT(XamContentClose)
{
    Args a{ ctx };
    const std::string root = reinterpret_cast<const char*>(g_base + a.u32(0));
    io::unlink(root + ":");
    a.ret(complete(a.u32(1), win32::success));
}

TT2_IMPORT(XamContentFlush)
{
    Args a{ ctx };
    a.ret(complete(a.u32(1), win32::success));
}

TT2_IMPORT(XamContentDelete)
{
    Args a{ ctx };
    std::error_code ec;
    fs::remove_all(xam::g_save_root / content_filename(a.u32(1)), ec);
    a.ret(complete(a.u32(2), win32::success));
}

TT2_IMPORT(XamContentGetCreator)
{
    Args a{ ctx };
    if (auto* c = a.ptr<be<uint32_t>>(2))
        *c = 1;
    if (auto* x = a.ptr<be<uint64_t>>(3))
        *x = kXuidBase + a.u32(0);
    a.ret(complete(a.u32(4), win32::success));
}

TT2_IMPORT(XamContentGetDeviceState)
{
    Args a{ ctx };
    a.ret(complete(a.u32(1), win32::success));
}

// XamContentCreateEnumerator(user, device, content_type, flags, items, buffer_size*, handle*)
TT2_IMPORT(XamContentCreateEnumerator)
{
    Args a{ ctx };
    auto e = std::make_shared<Enumerator>();
    e->item_size = 0x134;
    std::error_code ec;
    if (fs::exists(xam::g_save_root, ec))
    {
        for (auto& d : fs::directory_iterator(xam::g_save_root, ec))
        {
            if (!d.is_directory())
                continue;
            std::vector<uint8_t> item(0x134, 0);
            auto put32 = [&](size_t off, uint32_t v) {
                v = byteswap(v);
                std::memcpy(item.data() + off, &v, 4);
            };
            put32(0, 1);              // device: HDD
            put32(4, a.u32(2));       // content type
            const auto name = widen(d.path().filename().string());
            for (size_t i = 0; i < name.size() && i < 127; i++)
            {
                const uint16_t c = byteswap(uint16_t(name[i]));
                std::memcpy(item.data() + 8 + i * 2, &c, 2);
            }
            const std::string file = d.path().filename().string().substr(0, 41);
            std::memcpy(item.data() + 8 + 256, file.c_str(), file.size());
            e->items.push_back(std::move(item));
        }
    }
    if (auto* size = a.ptr<be<uint32_t>>(5))
        *size = 0x134 * std::max<uint32_t>(1, a.u32(4));
    if (auto* h = a.ptr<be<uint32_t>>(6))
        *h = create_handle(e);
    a.ret(win32::success);
}

// ---------------------------------------------------------------------- system UI

TT2_IMPORT(XamShowSigninUI) { ctx.r3.u64 = win32::success; }
TT2_IMPORT(XamShowFriendsUI) { ctx.r3.u64 = win32::success; }
TT2_IMPORT(XamShowGamerCardUIForXUID) { ctx.r3.u64 = win32::success; }
TT2_IMPORT(XamShowMarketplaceUI) { ctx.r3.u64 = win32::success; }
TT2_IMPORT(XamShowMarketplaceDownloadItemsUI) { ctx.r3.u64 = win32::success; }

// XamShowMessageBoxUI(user, title, text, button_count, buttons, focus, flags, result*, overlapped)
TT2_IMPORT(XamShowMessageBoxUI)
{
    Args a{ ctx };
    log("xam: message box \"%s\" -> button %u", "(guest text)", a.u32(5));
    if (auto* r = a.ptr<be<uint32_t>>(7))
        *r = a.u32(5);
    a.ret(complete(a.u32(8), win32::success));
}

TT2_IMPORT(XamShowMessageBoxUIEx)
{
    Args a{ ctx };
    if (auto* r = a.ptr<be<uint32_t>>(7))
        *r = a.u32(5);
    a.ret(complete(a.u32(8), win32::success));
}

// XamShowDeviceSelectorUI(user, content_type, flags, total_requested, device_id*, overlapped)
TT2_IMPORT(XamShowDeviceSelectorUI)
{
    Args a{ ctx };
    if (auto* d = a.ptr<be<uint32_t>>(4))
        *d = 1; // HDD
    a.ret(complete(a.u32(5), win32::success));
}

TT2_IMPORT(XamShowKeyboardUI)
{
    Args a{ ctx };
    a.ret(complete(a.u32(7), win32::cancelled));
}

// ---------------------------------------------------------------------- notifications

TT2_IMPORT(XamNotifyCreateListener)
{
    auto l = std::make_shared<Listener>(ctx.r3.u64);
    // What a console reports at boot: who is signed in, and that storage is present.
    uint32_t signed_in = 0;
    for (uint32_t u = 0; u < 2; u++)
        if (user_present(u))
            signed_in |= 1u << u;
    l->queue.push_back({ 0x0000000A, signed_in }); // XN_SYS_SIGNINCHANGED
    l->queue.push_back({ 0x0000000B, 0 });         // XN_SYS_STORAGEDEVICESCHANGED
    ctx.r3.u64 = create_handle(l);
}

// XNotifyGetNext(handle, match_id, id*, param*)
TT2_IMPORT(XNotifyGetNext)
{
    Args a{ ctx };
    auto l = std::dynamic_pointer_cast<Listener>(object_from_handle(a.u32(0)));
    if (!l)
        return a.ret(0);
    std::pair<uint32_t, uint32_t> n{};
    bool found = false;
    with_lock([&] {
        for (auto it = l->queue.begin(); it != l->queue.end(); ++it)
        {
            if (a.u32(1) == 0 || it->first == a.u32(1))
            {
                n = *it;
                l->queue.erase(it);
                found = true;
                break;
            }
        }
    });
    if (!found)
        return a.ret(0);
    if (auto* id = a.ptr<be<uint32_t>>(2))
        *id = n.first;
    if (auto* p = a.ptr<be<uint32_t>>(3))
        *p = n.second;
    a.ret(1);
}

// ---------------------------------------------------------------------- tasks

// XamTaskSchedule(callback, context, unknown, handle*)
TT2_IMPORT(XamTaskSchedule)
{
    Args a{ ctx };
    auto t = start_thread(a.u32(0), a.u32(1), 0, 0, false, "XamTask");
    if (auto* h = a.ptr<be<uint32_t>>(3))
        *h = create_handle(t);
    a.ret(status::success);
}

TT2_IMPORT(XamTaskCloseHandle)
{
    close_handle(ctx.r3.u32);
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(XamTaskShouldExit) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------- system info

TT2_IMPORT(XamGetExecutionId)
{
    // XEX_EXECUTION_ID: media id, version, base version, title id, platform, executable
    // type, disc number, disc count, savegame id
    static uint32_t info = 0;
    if (!info)
    {
        info = g_memory.system_alloc(0x18);
        store32(info + 0x0C, kTitleId);
        g_base[info + 0x12] = 1;
        g_base[info + 0x13] = 1;
    }
    if (auto* out = Args{ ctx }.ptr<be<uint32_t>>(0))
        *out = info;
    ctx.r3.u64 = status::success;
}

TT2_IMPORT(XamGetSystemVersion) { ctx.r3.u64 = 0x20096B00; }

TT2_IMPORT(XamLoaderTerminateTitle)
{
    log("game requested exit");
    std::exit(0);
}

TT2_IMPORT(XGetAVPack) { ctx.r3.u64 = 6; } // HDMI
TT2_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x00FF; }
TT2_IMPORT(XGetLanguage) { ctx.r3.u64 = 1; } // English
TT2_IMPORT(XGetVideoCapabilities) { ctx.r3.u64 = 0; }

// XVIDEO_MODE: width, height, interlaced, widescreen, hi-def, refresh (float), standard, reserved[5]
TT2_IMPORT(XGetVideoMode)
{
    const uint32_t m = ctx.r3.u32;
    std::memset(g_base + m, 0, 48);
    store32(m + 0, 1280);
    store32(m + 4, 720);
    store32(m + 8, 0);
    store32(m + 12, 1);
    store32(m + 16, 1);
    store32(m + 20, std::bit_cast<uint32_t>(60.0f));
    store32(m + 24, 1); // NTSC
}

TT2_IMPORT(XNetLogonGetTitleID) { ctx.r3.u64 = kTitleId; }

// ---------------------------------------------------------------------- XMsg / sessions / voice

TT2_IMPORT(XMsgInProcessCall) { ctx.r3.u64 = 0x80004005; }
TT2_IMPORT(XMsgStartIORequest)
{
    Args a{ ctx };
    a.ret(complete(a.u32(2), 0x80004005));
}
TT2_IMPORT(XMsgStartIORequestEx)
{
    Args a{ ctx };
    a.ret(complete(a.u32(2), 0x80004005));
}
TT2_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamSessionCreateHandle) { ctx.r3.u64 = 0x80004005; }
TT2_IMPORT(XamSessionRefObjByHandle) { ctx.r3.u64 = status::invalid_handle; }
TT2_IMPORT(XamVoiceCreate) { ctx.r3.u64 = win32::device_not_connected; }
TT2_IMPORT(XamVoiceClose) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceIsActiveProcess) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceSubmitPacket) { ctx.r3.u64 = win32::device_not_connected; }
TT2_IMPORT(XamParseGamerTileKey) { ctx.r3.u64 = win32::function_failed; }
TT2_IMPORT(XamReadTileToTexture) { ctx.r3.u64 = win32::function_failed; }
