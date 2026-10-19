// XAM: controllers, profiles, save content, system UI, notifications, tasks, video mode.
// There is no Xbox Live: online features report "offline", and netplay replaces them.

#include "objects.h"
#include "threads.h"
#include "io.h"
#include "../memory.h"
#include "../netplay_glue.h"
#include "../input/input.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

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
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
    constexpr uint32_t ERROR_NOT_LOGGED_ON = 1245;
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
        // XInput only changes the packet number when the state changes. Deriving it from the
        // state keeps it deterministic, so a re-simulated frame reads exactly what the original did.
        uint32_t packet = 2166136261u;
        for (uint32_t v : { uint32_t(s.buttons), uint32_t(s.left_trigger) << 8 | s.right_trigger, uint32_t(uint16_t(s.lx)) << 16 | uint16_t(s.ly),
                            uint32_t(uint16_t(s.rx)) << 16 | uint16_t(s.ry) })
            packet = (packet ^ v) * 16777619u;
        store32(state, packet | 1);
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

// XInputGetKeystroke reports pad buttons as virtual keys (VK_PAD_*); the game's menus confirm
// and cancel with them. Events come from changes in the same per-frame pad state that
// XamInputGetState returns, with key repeat for held buttons (as on the console: 400 ms, then
// every 100 ms). There is no keyboard.
namespace
{
    struct KeystrokeQueue
    {
        uint16_t previous = 0;
        uint8_t previous_triggers = 0; // bit 0 left, bit 1 right
        std::deque<std::pair<uint16_t, uint16_t>> events; // virtual key, flags
        uint16_t repeat_key = 0;
        std::chrono::steady_clock::time_point repeat_at;
    };
    KeystrokeQueue g_keys[4];
    std::mutex g_keys_mutex;

    constexpr uint16_t XINPUT_KEYSTROKE_KEYDOWN = 1, XINPUT_KEYSTROKE_KEYUP = 2, XINPUT_KEYSTROKE_REPEAT = 4;

    void poll_keystrokes(uint32_t user, const input::PadState& s)
    {
        static const std::pair<uint16_t, uint16_t> buttons[] = {
            { 0x1000, 0x5800 }, { 0x2000, 0x5801 }, { 0x4000, 0x5802 }, { 0x8000, 0x5803 }, // A B X Y
            { 0x0200, 0x5804 }, { 0x0100, 0x5805 },                                           // RB LB
            { 0x0001, 0x5810 }, { 0x0002, 0x5811 }, { 0x0004, 0x5812 }, { 0x0008, 0x5813 }, // d-pad
            { 0x0010, 0x5814 }, { 0x0020, 0x5815 }, { 0x0040, 0x5816 }, { 0x0080, 0x5817 }, // start back thumbs
        };
        KeystrokeQueue& q = g_keys[user];
        const auto now = std::chrono::steady_clock::now();
        const uint8_t triggers = uint8_t((s.left_trigger > 30 ? 1 : 0) | (s.right_trigger > 30 ? 2 : 0));
        auto change = [&](bool was, bool is, uint16_t vk) {
            if (is && !was)
            {
                q.events.push_back({ vk, XINPUT_KEYSTROKE_KEYDOWN });
                q.repeat_key = vk;
                q.repeat_at = now + std::chrono::milliseconds(400);
            }
            else if (!is && was)
            {
                q.events.push_back({ vk, XINPUT_KEYSTROKE_KEYUP });
                if (q.repeat_key == vk)
                    q.repeat_key = 0;
            }
        };
        for (auto [bit, vk] : buttons)
            change(q.previous & bit, s.buttons & bit, vk);
        change(q.previous_triggers & 1, triggers & 1, 0x5806);
        change(q.previous_triggers & 2, triggers & 2, 0x5807);
        q.previous = s.buttons;
        q.previous_triggers = triggers;
        if (q.repeat_key && now >= q.repeat_at && q.events.empty())
        {
            q.events.push_back({ q.repeat_key, XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT });
            q.repeat_at = now + std::chrono::milliseconds(100);
        }
    }
}

// XamInputGetKeystrokeEx(user_index* (0xFF: any), flags, XINPUT_KEYSTROKE*)
// XINPUT_KEYSTROKE: be16 virtual key, be16 unicode, be16 flags, u8 user index, u8 hid code
TT2_IMPORT(XamInputGetKeystrokeEx)
{
    Args a{ ctx };
    const uint32_t user_ptr = a.u32(0), out = a.u32(2);
    const uint32_t requested = user_ptr ? load32(user_ptr) : 0;
    std::lock_guard lock(g_keys_mutex);
    bool any_connected = false;
    for (uint32_t user = 0; user < 4; user++)
    {
        if (requested != 0xFF && requested != user)
            continue;
        input::PadState s;
        if (!netglue::input_for_port(int(user), s))
            s = input::read(int(user));
        if (!s.connected)
            continue;
        any_connected = true;
        poll_keystrokes(user, s);
        KeystrokeQueue& q = g_keys[user];
        if (q.events.empty())
            continue;
        const auto [vk, flags] = q.events.front();
        q.events.pop_front();
        if (out)
        {
            store16(out + 0, vk);
            store16(out + 2, 0);
            store16(out + 4, flags);
            g_base[out + 6] = uint8_t(user);
            g_base[out + 7] = 0;
        }
        if (user_ptr)
            store32(user_ptr, user);
        return a.ret(win32::success);
    }
    a.ret(any_connected ? ERROR_EMPTY : win32::device_not_connected);
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
//
// No profile settings are stored, so every requested setting comes back as "default" (from = 0)
// with an empty value of the setting's type, and the game uses its own defaults. Like the
// console, a call without a big enough buffer only reports the size needed.
// Result: be32 count, be32 settings pointer, then count X_USER_PROFILE_SETTING (40 bytes: from,
// pad, user index / xuid, setting id, pad, X_USER_DATA {type, pad, value or {length, pointer}}),
// then the space for binary values.
TT2_IMPORT(XamUserReadProfileSettings)
{
    Args a{ ctx };
    const uint32_t user = a.u32(1), count = a.u32(4), ids = a.u32(5), size_ptr = a.u32(6), buffer = a.u32(7);
    if (a.u32(2) == 0 && !user_present(user))
    {
        a.ret(complete(a.u32(8), ERROR_NOT_LOGGED_ON));
        return;
    }
    auto binary_size = [](uint32_t id) { return (id >> 16) & 0xFFF; };
    uint32_t needed = 8 + count * 40;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = load32(ids + i * 4);
        if (((id >> 28) & 0xF) == 6 || ((id >> 28) & 0xF) == 4) // binary, unicode string
            needed += (binary_size(id) + 3) & ~3u;
    }
    const uint32_t available = size_ptr ? load32(size_ptr) : 0;
    if (size_ptr)
        store32(size_ptr, needed);
    if (!buffer || available < needed)
    {
        a.ret(ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    std::memset(g_base + buffer, 0, needed);
    store32(buffer + 0, count);
    store32(buffer + 4, buffer + 8);
    uint32_t extra = buffer + 8 + count * 40;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t id = load32(ids + i * 4);
        const uint32_t s = buffer + 8 + i * 40;
        const uint32_t type = (id >> 28) & 0xF;
        store32(s + 0, 0);      // from: default
        store32(s + 8, user);   // user index
        store32(s + 16, id);
        g_base[s + 24] = uint8_t(type);
        if (type == 6 || type == 4)
        {
            store32(s + 32, 0); // length
            store32(s + 36, extra);
            extra += (binary_size(id) + 3) & ~3u;
        }
    }
    a.ret(complete(a.u32(8), win32::success));
}

TT2_IMPORT(XamUserGetDeviceContext)
{
    Args a{ ctx };
    if (auto* r = a.ptr<be<uint32_t>>(2))
        *r = 0;
    a.ret(user_present(a.u32(0)) ? win32::success : win32::device_not_connected);
}

// XamUserCreateAchievementEnumerator(title, user, xuid, detail_flags, start, count, buffer_size*, handle*)
// The buffer size is count entries of a full XACHIEVEMENT_DETAILS with its strings (500 bytes
// each); TTT2 asserts on exactly that. No achievements are enumerated.
TT2_IMPORT(XamUserCreateAchievementEnumerator)
{
    Args a{ ctx };
    auto e = std::make_shared<Enumerator>();
    if (auto* size = a.ptr<be<uint32_t>>(6))
        *size = a.u32(5) * 500;
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

// DLC (marketplace content, type 2): the player's own packages, which setup verifies and
// extracts into native\dlc\<package file name>\ with an info.json. Each holds an index.dat
// and the data archive the game loads from it (DLCDATnnn:\dataNNN.dat). They are reported as
// installed and mounted read-only when the game opens them. TT2_DLC=0 hides them.
namespace
{
    constexpr uint32_t XCONTENTTYPE_MARKETPLACE = 2;

    struct DlcPackage
    {
        std::string file_name;    // the package's name, XCONTENT_DATA.FileName
        std::string display_name;
        fs::path dir;
    };

    const std::vector<DlcPackage>& installed_dlc()
    {
        static const std::vector<DlcPackage> packages = [] {
            std::vector<DlcPackage> list;
            if (const char* v = std::getenv("TT2_DLC"); v && *v == '0')
                return list;
            std::error_code ec;
            const fs::path root = xam::g_save_root.parent_path() / "dlc";
            for (auto& d : fs::directory_iterator(root, ec))
            {
                const fs::path info = d.path() / "info.json";
                if (!d.is_directory() || !fs::exists(info, ec))
                    continue;
                try
                {
                    const auto j = nlohmann::json::parse(std::ifstream(info));
                    DlcPackage pkg{ j.at("file_name").get<std::string>(), j.value("display_name", std::string()), d.path() };
                    if (pkg.file_name.size() > 42)
                        continue;
                    log("xam: DLC installed: %s", pkg.display_name.c_str());
                    list.push_back(std::move(pkg));
                }
                catch (const std::exception& e)
                {
                    log("xam: ignoring %s: %s", info.string().c_str(), e.what());
                }
            }
            return list;
        }();
        return packages;
    }
}

// XamContentCreateEx(user, root_name, content_data, flags, disposition*, license*,
//                    cache_size, content_size, overlapped)
TT2_IMPORT(XamContentCreateEx)
{
    Args a{ ctx };
    const std::string root = reinterpret_cast<const char*>(g_base + a.u32(1));
    const std::string file = content_filename(a.u32(2));
    const uint32_t flags = a.u32(3);
    // XCONTENT_DATA: be32 device, be32 content type, ... Saved games (type 1) and installed DLC.
    if (a.u32(2) && load32(a.u32(2) + 4) == XCONTENTTYPE_MARKETPLACE)
    {
        const DlcPackage* pkg = nullptr;
        for (const DlcPackage& d : installed_dlc())
            if (d.file_name == file)
                pkg = &d;
        if (!pkg)
            return a.ret(complete(a.u32(8), win32::file_not_found));
        io::mount("\\device\\content\\" + root, pkg->dir, true);
        io::link(root + ":", "\\device\\content\\" + root);
        if (auto* d = a.ptr<be<uint32_t>>(4))
            *d = 2; // XCONTENT_EXISTING
        if (auto* l = a.ptr<be<uint32_t>>(5))
            *l = 0xFFFFFFFF; // licensed
        return a.ret(complete(a.u32(8), win32::success));
    }
    if (a.u32(2) && load32(a.u32(2) + 4) != 1)
        return a.ret(complete(a.u32(8), win32::file_not_found));
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
// Only saved games (content type 1) exist: they are the folders in native\save. Anything else,
// such as marketplace content (type 2, the DLC characters' unlock packages), enumerates as
// nothing installed. Reporting the save folders for every type made the game load the save
// data as DLC packages.
TT2_IMPORT(XamContentCreateEnumerator)
{
    Args a{ ctx };
    auto e = std::make_shared<Enumerator>();
    e->item_size = 0x134;
    std::error_code ec;
    constexpr uint32_t XCONTENTTYPE_SAVEDGAME = 1;
    // Installed DLC is device content: listed once, for the first user's enumeration (the game
    // enumerates users 0-3 and would see every package four times).
    if (a.u32(2) == XCONTENTTYPE_MARKETPLACE && a.u32(0) == 0)
        for (const DlcPackage& pkg : installed_dlc())
        {
            std::vector<uint8_t> item(0x134, 0);
            auto put32 = [&](size_t off, uint32_t v) {
                v = byteswap(v);
                std::memcpy(item.data() + off, &v, 4);
            };
            put32(0, 1); // device: HDD
            put32(4, XCONTENTTYPE_MARKETPLACE);
            const auto name = widen(pkg.display_name);
            for (size_t i = 0; i < name.size() && i < 127; i++)
            {
                const uint16_t c = byteswap(uint16_t(name[i]));
                std::memcpy(item.data() + 8 + i * 2, &c, 2);
            }
            std::memcpy(item.data() + 8 + 256, pkg.file_name.data(), pkg.file_name.size());
            e->items.push_back(std::move(item));
        }
    if (a.u32(2) == XCONTENTTYPE_SAVEDGAME && fs::exists(xam::g_save_root, ec))
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

namespace
{
    constexpr uint32_t kXmpApp = 0xFA; // background music player ("XMP")
    constexpr uint32_t X_E_FAIL = 0x80004005;

    // XMP messages, following Xenia's xmp_app.cc. The argument block starts with the XMP
    // client id, followed by pointers to the outputs. No user music is ever playing: the
    // game owns playback.
    uint32_t xmp_message(uint32_t message, uint32_t args)
    {
        auto out32 = [&](int index, uint32_t value) {
            if (args)
                if (const uint32_t p = load32(args + index * 4))
                    store32(p, value);
        };
        switch (message)
        {
        case 0x00070009: out32(1, 0); return 0;                                // XMPGetStatus: idle
        case 0x0007000B: out32(1, std::bit_cast<uint32_t>(1.0f)); return 0;    // XMPGetVolume
        case 0x0007001B: out32(1, 1); out32(2, 0); return 0;                   // XMPGetPlaybackController: title, not locked
        case 0x00070029: out32(1, 0); out32(2, 0); out32(3, 0); return 0;      // XMPGetPlaybackBehavior
        case 0x00070053: out32(1, 0); return 0;                                // XMPGetDashInitState
        case 0x00070002: case 0x00070003: case 0x00070004: case 0x00070005:     // play/continue/stop/pause
        case 0x00070006: case 0x00070007: case 0x00070008: case 0x0007000C:     // next/prev/behavior/volume
        case 0x0007001A: case 0x00070013: case 0x0007003D:
            return 0;
        default:
            log("xam: XMP message %08X not implemented", message);
            return X_E_FAIL;
        }
    }

    uint32_t dispatch_message(uint32_t app, uint32_t message, uint32_t args)
    {
        if (app == kXmpApp)
            return xmp_message(message, args);
        log("xam: message %08X for app %02X not implemented", message, app);
        return X_E_FAIL;
    }
}

// XMsgInProcessCall(app, message, param1, param2)
TT2_IMPORT(XMsgInProcessCall)
{
    Args a{ ctx };
    a.ret(dispatch_message(a.u32(0), a.u32(1), a.u32(2)));
}

// XMsgStartIORequest(app, message, overlapped, buffer, buffer_size)
TT2_IMPORT(XMsgStartIORequest)
{
    Args a{ ctx };
    a.ret(complete(a.u32(2), dispatch_message(a.u32(0), a.u32(1), a.u32(3))));
}

// XMsgStartIORequestEx(app, message, overlapped, buffer, buffer_size, unknown)
TT2_IMPORT(XMsgStartIORequestEx)
{
    Args a{ ctx };
    a.ret(complete(a.u32(2), dispatch_message(a.u32(0), a.u32(1), a.u32(3))));
}
TT2_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamSessionCreateHandle) { ctx.r3.u64 = 0x80004005; }
TT2_IMPORT(XamSessionRefObjByHandle) { ctx.r3.u64 = status::invalid_handle; }
TT2_IMPORT(XamVoiceCreate) { ctx.r3.u64 = win32::device_not_connected; }
TT2_IMPORT(XamVoiceClose) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceIsActiveProcess) { ctx.r3.u64 = 0; }
TT2_IMPORT(XamVoiceSubmitPacket) { ctx.r3.u64 = win32::device_not_connected; }
// Gamer pictures: there are none. The key parses to tile 0 and reading a tile succeeds without
// drawing anything (as in Xenia); failing makes the game retry every frame.
// XamParseGamerTileKey(key, title_id*, big_tile_id*, small_tile_id*)
TT2_IMPORT(XamParseGamerTileKey)
{
    Args a{ ctx };
    for (int i = 1; i <= 3; i++)
        if (auto* p = a.ptr<be<uint32_t>>(i))
            *p = 0;
    a.ret(win32::success);
}

// XamReadTileToTexture(type, title_id, tile_id, user, buffer, stride, height, overlapped)
TT2_IMPORT(XamReadTileToTexture)
{
    Args a{ ctx };
    a.ret(complete(a.u32(7), win32::success));
}
