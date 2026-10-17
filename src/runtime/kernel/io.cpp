// File system imports. Xbox device paths are mapped onto folders inside native\:
//   \Device\Cdrom0 (game:, d:)          -> native\game        (read-only, the verified disc)
//   \Device\Harddisk0\Partition1        -> native\hdd
//   cache:, \Device\Harddisk0\Cache0    -> native\cache
// plus whatever the game links with ObCreateSymbolicLink and content packages it mounts.

#include "objects.h"
#include "io.h"
#include "../memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;
using namespace tt2;
using namespace tt2::kernel;

namespace tt2::io
{
    namespace
    {
        std::mutex g_mutex;
        std::map<std::string, std::string> g_links;   // "game:" -> "\device\cdrom0"
        std::map<std::string, fs::path> g_devices;    // "\device\cdrom0" -> host folder
        std::map<std::string, bool> g_read_only;

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }
    }

    void mount(const std::string& device, const fs::path& host, bool read_only)
    {
        std::lock_guard lock(g_mutex);
        g_devices[lower(device)] = host;
        g_read_only[lower(device)] = read_only;
    }

    void link(const std::string& name, const std::string& target)
    {
        std::lock_guard lock(g_mutex);
        g_links[lower(name)] = lower(target);
    }

    void unlink(const std::string& name)
    {
        std::lock_guard lock(g_mutex);
        g_links.erase(lower(name));
    }

    std::optional<Resolved> resolve(std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        std::string l = lower(path);
        if (l.rfind("\\??\\", 0) == 0)
            l = l.substr(4);

        std::lock_guard lock(g_mutex);
        // Symbolic links ("game:\x" or "\??\game:\x")
        for (int depth = 0; depth < 4; depth++)
        {
            const auto colon = l.find(':');
            if (colon == std::string::npos || l.find('\\') < colon)
                break;
            const auto it = g_links.find(l.substr(0, colon + 1));
            if (it == g_links.end())
                break;
            l = it->second + l.substr(colon + 1);
        }

        // Longest matching device prefix
        const std::pair<const std::string, fs::path>* best = nullptr;
        for (const auto& d : g_devices)
            if (l.rfind(d.first, 0) == 0 && (l.size() == d.first.size() || l[d.first.size()] == '\\') &&
                (!best || d.first.size() > best->first.size()))
                best = &d;
        if (!best)
            return std::nullopt;

        // Lower-cased remainder: the host file system is case-insensitive like FATX.
        std::string rest = l.substr(best->first.size());
        while (!rest.empty() && rest.front() == '\\')
            rest.erase(rest.begin());
        // Never let "..\" escape the mounted folder.
        fs::path host = best->second;
        for (const auto& part : fs::path(rest))
        {
            if (part == "..")
                return std::nullopt;
            if (part != "." && !part.empty())
                host /= part;
        }
        return Resolved{ host, g_read_only[best->first] };
    }

    std::string read_ansi_string(uint32_t ansi_string)
    {
        if (!ansi_string)
            return {};
        const uint16_t length = load16(ansi_string);
        const uint32_t buffer = load32(ansi_string + 4);
        return buffer ? std::string(reinterpret_cast<const char*>(g_base + buffer), length) : std::string();
    }

    std::string object_attributes_path(uint32_t attributes)
    {
        // OBJECT_ATTRIBUTES: RootDirectory, ObjectName (ANSI_STRING*), Attributes
        return attributes ? read_ansi_string(load32(attributes + 4)) : std::string();
    }
}

namespace
{
    struct File final : Object
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        fs::path path;
        bool directory = false;
        uint64_t position = 0;
        std::vector<fs::directory_entry> listing;
        size_t listing_pos = 0;
        bool listed = false;

        ObjectType type() const override { return ObjectType::File; }
        bool is_signaled(uint32_t) const override { return true; }
        ~File() override
        {
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    };

    constexpr uint32_t XFILE_SUPERSEDE = 0, XFILE_OPEN = 1, XFILE_CREATE = 2, XFILE_OPEN_IF = 3, XFILE_OVERWRITE = 4, XFILE_OVERWRITE_IF = 5;
    constexpr uint32_t XFILE_DIRECTORY_FILE = 0x1;
    constexpr uint32_t XFILE_OPENED = 1, XFILE_CREATED = 2, XFILE_OVERWRITTEN = 3, XFILE_DOES_NOT_EXIST = 5;
    constexpr uint32_t XFILE_ATTRIBUTE_READONLY = 0x1, XFILE_ATTRIBUTE_DIRECTORY = 0x10, XFILE_ATTRIBUTE_NORMAL = 0x80;

    void set_io_status(uint32_t io, uint32_t status, uint32_t information)
    {
        if (io)
        {
            store32(io, status);
            store32(io + 4, information);
        }
    }

    uint64_t filetime(const fs::path& p)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data))
            return 0;
        return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }

    // FILE_NETWORK_OPEN_INFORMATION: 4 times, allocation size, end of file, attributes
    void write_network_open_info(uint32_t out, const fs::path& p, bool read_only)
    {
        std::error_code ec;
        const bool dir = fs::is_directory(p, ec);
        const uint64_t size = dir ? 0 : fs::file_size(p, ec);
        const uint64_t t = filetime(p);
        for (int i = 0; i < 4; i++)
            store64(out + i * 8, t);
        store64(out + 0x20, (size + 4095) & ~4095ull);
        store64(out + 0x28, size);
        store32(out + 0x30, dir ? XFILE_ATTRIBUTE_DIRECTORY : (read_only ? XFILE_ATTRIBUTE_READONLY : XFILE_ATTRIBUTE_NORMAL));
        store32(out + 0x34, 0);
    }

    uint32_t open_file(const Args& a, uint32_t attributes, uint32_t io, uint32_t disposition, uint32_t options, uint32_t desired)
    {
        const std::string guest_path = io::object_attributes_path(attributes);
        const auto resolved = io::resolve(guest_path);
        if (!resolved)
        {
            log("io: no device for '%s'", guest_path.c_str());
            set_io_status(io, status::object_path_not_found, XFILE_DOES_NOT_EXIST);
            return status::object_path_not_found;
        }

        std::error_code ec;
        const bool exists = fs::exists(resolved->host, ec);
        const bool is_dir = exists && fs::is_directory(resolved->host, ec);
        const bool want_write = (desired & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0; // GENERIC_WRITE, FILE_WRITE_DATA, APPEND, GENERIC_ALL

        if (!exists && (disposition == XFILE_OPEN || disposition == XFILE_OVERWRITE))
        {
            set_io_status(io, status::object_name_not_found, XFILE_DOES_NOT_EXIST);
            return status::object_name_not_found;
        }
        if (exists && disposition == XFILE_CREATE)
        {
            set_io_status(io, status::object_name_collision, 0);
            return status::object_name_collision;
        }
        if (resolved->read_only && (!exists || (want_write && disposition != XFILE_OPEN)))
        {
            set_io_status(io, status::access_denied, 0);
            return status::access_denied;
        }

        auto f = std::make_shared<File>();
        f->path = resolved->host;
        uint32_t info = exists ? XFILE_OPENED : XFILE_CREATED;

        if (is_dir || (!exists && (options & XFILE_DIRECTORY_FILE)))
        {
            if (!exists)
                fs::create_directories(resolved->host, ec);
            f->directory = true;
        }
        else
        {
            if (!exists)
                fs::create_directories(resolved->host.parent_path(), ec);
            DWORD creation = OPEN_EXISTING;
            switch (disposition)
            {
            case XFILE_SUPERSEDE:
            case XFILE_OVERWRITE_IF: creation = CREATE_ALWAYS; info = exists ? XFILE_OVERWRITTEN : XFILE_CREATED; break;
            case XFILE_CREATE: creation = CREATE_NEW; break;
            case XFILE_OPEN_IF: creation = OPEN_ALWAYS; break;
            case XFILE_OVERWRITE: creation = TRUNCATE_EXISTING; info = XFILE_OVERWRITTEN; break;
            default: creation = OPEN_EXISTING; break;
            }
            const DWORD access = GENERIC_READ | (resolved->read_only ? 0 : GENERIC_WRITE);
            f->h = CreateFileW(resolved->host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f->h == INVALID_HANDLE_VALUE)
            {
                log("io: cannot open %s (error %lu)", resolved->host.string().c_str(), GetLastError());
                set_io_status(io, status::access_denied, 0);
                return status::access_denied;
            }
        }

        if (auto* h = a.ptr<be<uint32_t>>(0))
            *h = create_handle(f);
        set_io_status(io, status::success, info);
        return status::success;
    }

    // Shared by NtReadFile / NtWriteFile: completes synchronously, then signals the event.
    uint32_t transfer(const Args& a, bool write, uint32_t buffer, uint32_t length, const be<int64_t>* offset)
    {
        auto f = handle_as<File>(a.u32(0));
        const uint32_t io = a.u32(4);
        if (!f || f->directory)
            return status::invalid_handle;

        uint64_t pos = f->position;
        if (offset && int64_t(*offset) >= 0)
            pos = uint64_t(int64_t(*offset));

        OVERLAPPED ov{};
        ov.Offset = DWORD(pos);
        ov.OffsetHigh = DWORD(pos >> 32);
        DWORD done = 0;
        const BOOL ok = write ? WriteFile(f->h, g_base + buffer, length, &done, &ov) : ReadFile(f->h, g_base + buffer, length, &done, &ov);
        uint32_t result = status::success;
        if (!ok && GetLastError() != ERROR_HANDLE_EOF)
            result = status::unsuccessful;
        else if (!write && done == 0 && length != 0)
            result = status::end_of_file;
        f->position = pos + done;
        set_io_status(io, result, done);

        if (auto e = std::dynamic_pointer_cast<Event>(object_from_handle(a.u32(1))))
            with_lock([&] {
                e->signaled = true;
                e->mirror_signal_state(1);
            });
        if (a.u32(2))
            log("io: completion APC routines are not delivered");
        return result;
    }
}

TT2_IMPORT(NtCreateFile)
{
    Args a{ ctx };
    a.ret(open_file(a, a.u32(2), a.u32(3), a.u32(7), a.u32(8), a.u32(1)));
}

TT2_IMPORT(NtOpenFile)
{
    Args a{ ctx };
    a.ret(open_file(a, a.u32(2), a.u32(3), XFILE_OPEN, a.u32(5), a.u32(1)));
}

// NtReadFile(handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset*)
TT2_IMPORT(NtReadFile)
{
    Args a{ ctx };
    a.ret(transfer(a, false, a.u32(5), a.u32(6), a.ptr<be<int64_t>>(7)));
}

TT2_IMPORT(NtWriteFile)
{
    Args a{ ctx };
    a.ret(transfer(a, true, a.u32(5), a.u32(6), a.ptr<be<int64_t>>(7)));
}

// Scatter/gather: the "buffer" is an array of 4 KB page addresses.
static uint32_t scatter_gather(const Args& a, bool write)
{
    const uint32_t segments = a.u32(5);
    const uint32_t length = a.u32(6);
    const auto* offset = a.ptr<be<int64_t>>(7);
    auto f = handle_as<File>(a.u32(0));
    if (!f)
        return status::invalid_handle;
    int64_t pos = offset && int64_t(*offset) >= 0 ? int64_t(*offset) : int64_t(f->position);
    uint32_t total = 0, result = status::success;
    for (uint32_t done = 0; done < length; done += 4096)
    {
        const uint32_t page = load32(segments + (done / 4096) * 4);
        const uint32_t chunk = std::min<uint32_t>(4096, length - done);
        OVERLAPPED ov{};
        ov.Offset = DWORD(pos);
        ov.OffsetHigh = DWORD(uint64_t(pos) >> 32);
        DWORD n = 0;
        const BOOL ok = write ? WriteFile(f->h, g_base + page, chunk, &n, &ov) : ReadFile(f->h, g_base + page, chunk, &n, &ov);
        total += n;
        pos += n;
        if (!ok || n < chunk)
        {
            if (!write && total == 0)
                result = status::end_of_file;
            break;
        }
    }
    f->position = uint64_t(pos);
    set_io_status(a.u32(4), result, total);
    if (auto e = std::dynamic_pointer_cast<Event>(object_from_handle(a.u32(1))))
        with_lock([&] { e->signaled = true; });
    return result;
}

TT2_IMPORT(NtReadFileScatter)
{
    Args a{ ctx };
    a.ret(scatter_gather(a, false));
}

TT2_IMPORT(NtWriteFileGather)
{
    Args a{ ctx };
    a.ret(scatter_gather(a, true));
}

// NtQueryInformationFile(handle, io_status, info*, length, class)
TT2_IMPORT(NtQueryInformationFile)
{
    Args a{ ctx };
    auto f = handle_as<File>(a.u32(0));
    const uint32_t io = a.u32(1), out = a.u32(2), length = a.u32(3), cls = a.u32(4);
    if (!f)
        return a.ret(status::invalid_handle);

    std::error_code ec;
    const uint64_t size = f->directory ? 0 : fs::file_size(f->path, ec);
    uint32_t written = 0;
    switch (cls)
    {
    case 4: // FileBasicInformation: 4 times + attributes
        for (int i = 0; i < 4; i++)
            store64(out + i * 8, filetime(f->path));
        store32(out + 0x20, f->directory ? XFILE_ATTRIBUTE_DIRECTORY : XFILE_ATTRIBUTE_NORMAL);
        written = 0x28;
        break;
    case 5: // FileStandardInformation
        store64(out, (size + 4095) & ~4095ull);
        store64(out + 8, size);
        store32(out + 0x10, 1);
        g_base[out + 0x14] = 0;
        g_base[out + 0x15] = f->directory ? 1 : 0;
        written = 0x18;
        break;
    case 6: // FileInternalInformation
        store64(out, std::hash<std::wstring>{}(f->path.wstring()));
        written = 8;
        break;
    case 14: // FilePositionInformation
        store64(out, f->position);
        written = 8;
        break;
    case 20: // FileEndOfFileInformation
        store64(out, size);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        write_network_open_info(out, f->path, false);
        written = 0x38;
        break;
    default:
        log("io: NtQueryInformationFile class %u not implemented", cls);
        set_io_status(io, status::not_implemented, 0);
        return a.ret(status::not_implemented);
    }
    (void)length;
    set_io_status(io, status::success, written);
    a.ret(status::success);
}

// NtSetInformationFile(handle, io_status, info*, length, class)
TT2_IMPORT(NtSetInformationFile)
{
    Args a{ ctx };
    auto f = handle_as<File>(a.u32(0));
    const uint32_t io = a.u32(1), in = a.u32(2), cls = a.u32(4);
    if (!f)
        return a.ret(status::invalid_handle);
    switch (cls)
    {
    case 14: // FilePositionInformation
        f->position = load64(in);
        break;
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
    {
        LARGE_INTEGER li;
        li.QuadPart = int64_t(load64(in));
        if (f->h != INVALID_HANDLE_VALUE && SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN))
            SetEndOfFile(f->h);
        break;
    }
    case 13: // FileDispositionInformation: delete on close
        if (g_base[in])
        {
            if (f->h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(f->h);
                f->h = INVALID_HANDLE_VALUE;
            }
            std::error_code ec;
            fs::remove(f->path, ec);
        }
        break;
    default:
        log("io: NtSetInformationFile class %u ignored", cls);
        break;
    }
    set_io_status(io, status::success, 0);
    a.ret(status::success);
}

// NtQueryFullAttributesFile(object_attributes*, FILE_NETWORK_OPEN_INFORMATION* out)
TT2_IMPORT(NtQueryFullAttributesFile)
{
    Args a{ ctx };
    const auto resolved = io::resolve(io::object_attributes_path(a.u32(0)));
    std::error_code ec;
    if (!resolved || !fs::exists(resolved->host, ec))
        return a.ret(status::object_name_not_found);
    write_network_open_info(a.u32(1), resolved->host, resolved->read_only);
    a.ret(status::success);
}

// NtQueryVolumeInformationFile(handle, io_status, info*, length, class)
TT2_IMPORT(NtQueryVolumeInformationFile)
{
    Args a{ ctx };
    const uint32_t io = a.u32(1), out = a.u32(2), length = a.u32(3), cls = a.u32(4);
    uint32_t written = 0;
    switch (cls)
    {
    case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        store64(out, 0);
        store32(out + 8, 0x54543250);
        store32(out + 12, 0);
        g_base[out + 16] = 0;
        written = 18;
        break;
    case 3: // FileFsSizeInformation
        store64(out, 0x1000000);  // total allocation units
        store64(out + 8, 0x800000); // available
        store32(out + 16, 8);       // sectors per unit
        store32(out + 20, 512);     // bytes per sector
        written = 24;
        break;
    case 5: // FileFsAttributeInformation: attributes, max component length, name length, name
        store32(out, 0);
        store32(out + 4, 255);
        store32(out + 8, 4);
        if (length >= 16)
            std::memcpy(g_base + out + 12, "FATX", 4);
        written = 16;
        break;
    default:
        log("io: NtQueryVolumeInformationFile class %u not implemented", cls);
        return a.ret(status::not_implemented);
    }
    set_io_status(io, status::success, written);
    a.ret(status::success);
}

// NtQueryDirectoryFile(handle, event, apc, apc_context, io_status, info*, length, mask ANSI_STRING*, restart)
TT2_IMPORT(NtQueryDirectoryFile)
{
    Args a{ ctx };
    auto f = handle_as<File>(a.u32(0));
    const uint32_t io = a.u32(4), out = a.u32(5), length = a.u32(6);
    if (!f || !f->directory)
        return a.ret(status::invalid_handle);

    if (!f->listed || a.u32(8))
    {
        f->listing.clear();
        std::string mask = io::read_ansi_string(a.u32(7));
        std::error_code ec;
        for (auto& e : fs::directory_iterator(f->path, ec))
        {
            if (!mask.empty() && mask != "*" && mask != "*.*")
            {
                // simple masks only: exact name or "*.ext"
                const std::string name = e.path().filename().string();
                if (mask.rfind("*", 0) == 0 ? !name.ends_with(mask.substr(1)) : _stricmp(name.c_str(), mask.c_str()) != 0)
                    continue;
            }
            f->listing.push_back(e);
        }
        f->listing_pos = 0;
        f->listed = true;
    }

    if (f->listing_pos >= f->listing.size())
    {
        set_io_status(io, status::no_more_files, 0);
        return a.ret(status::no_more_files);
    }

    // FILE_DIRECTORY_INFORMATION (ANSI name on Xbox), one entry per call
    const auto& e = f->listing[f->listing_pos];
    const std::string name = e.path().filename().string();
    const uint32_t needed = 0x40 + uint32_t(name.size());
    if (length < needed)
        return a.ret(status::buffer_overflow);
    std::error_code ec;
    const bool dir = e.is_directory(ec);
    const uint64_t size = dir ? 0 : e.file_size(ec);
    const uint64_t t = filetime(e.path());
    store32(out, 0);
    store32(out + 4, uint32_t(f->listing_pos));
    for (int i = 0; i < 4; i++)
        store64(out + 8 + i * 8, t);
    store64(out + 0x28, size);
    store64(out + 0x30, (size + 4095) & ~4095ull);
    store32(out + 0x38, dir ? XFILE_ATTRIBUTE_DIRECTORY : XFILE_ATTRIBUTE_NORMAL);
    store32(out + 0x3C, uint32_t(name.size()));
    std::memcpy(g_base + out + 0x40, name.data(), name.size());
    f->listing_pos++;
    set_io_status(io, status::success, needed);
    a.ret(status::success);
}

TT2_IMPORT(NtFlushBuffersFile)
{
    Args a{ ctx };
    if (auto f = handle_as<File>(a.u32(0)); f && f->h != INVALID_HANDLE_VALUE)
        FlushFileBuffers(f->h);
    set_io_status(a.u32(1), status::success, 0);
    a.ret(status::success);
}

TT2_IMPORT(NtDeviceIoControlFile)
{
    Args a{ ctx };
    log("io: NtDeviceIoControlFile code %08X not supported", a.u32(5));
    a.ret(status::not_supported);
}

TT2_IMPORT(ObCreateSymbolicLink)
{
    Args a{ ctx };
    const std::string name = io::read_ansi_string(a.u32(0));
    const std::string target = io::read_ansi_string(a.u32(1));
    io::link(name.rfind("\\??\\", 0) == 0 ? name.substr(4) : name, target);
    a.ret(status::success);
}

TT2_IMPORT(ObDeleteSymbolicLink)
{
    Args a{ ctx };
    const std::string name = io::read_ansi_string(a.u32(0));
    io::unlink(name.rfind("\\??\\", 0) == 0 ? name.substr(4) : name);
    a.ret(status::success);
}

TT2_IMPORT(IoDismountVolume) { ctx.r3.u64 = status::success; }
TT2_IMPORT(IoDismountVolumeByFileHandle) { ctx.r3.u64 = status::success; }
