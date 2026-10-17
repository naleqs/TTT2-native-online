// Rtl string/time helpers, the CRT printf family the kernel exports, and exception stubs.

#include "../guest.h"
#include "../image.h"
#include "../memory.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace tt2;

namespace
{
    // ------------------------------------------------------------------ varargs
    // Variadic calls pass every argument in an 8-byte slot: r3-r10, then the caller's
    // parameter area at r1+0x54 (32-bit) / r1+0x50 (64-bit). Doubles travel in the integer
    // slots as raw bits. A va_list is a pointer to such 8-byte slots.
    struct ArgSource
    {
        virtual ~ArgSource() = default;
        virtual uint64_t next64() = 0;
        uint32_t next32() { return uint32_t(next64()); }
        double next_double() { return std::bit_cast<double>(next64()); }
    };

    struct RegisterArgs final : ArgSource
    {
        Args args;
        int index;
        RegisterArgs(PPCContext& ctx, int first) : args{ ctx }, index(first) {}
        uint64_t next64() override { return args.u64(index++); }
    };

    struct VaListArgs final : ArgSource
    {
        uint32_t ptr;
        explicit VaListArgs(uint32_t p) : ptr(p) {}
        uint64_t next64() override
        {
            const uint64_t v = load64(ptr);
            ptr += 8;
            return v;
        }
    };

    std::u16string read_wide(uint32_t p, size_t max = 0x10000)
    {
        std::u16string s;
        for (size_t i = 0; p && i < max; i++)
        {
            const char16_t c = char16_t(load16(p + uint32_t(i) * 2));
            if (!c)
                break;
            s.push_back(c);
        }
        return s;
    }

    std::string narrow(const std::u16string& w)
    {
        std::string s;
        for (char16_t c : w)
            s.push_back(c < 0x80 ? char(c) : '?');
        return s;
    }

    // printf with guest arguments. `wide_format`: the format string and %s are wide.
    std::u16string format(const std::u16string& fmt, ArgSource& args, bool wide_format)
    {
        std::u16string out;
        for (size_t i = 0; i < fmt.size(); i++)
        {
            if (fmt[i] != u'%')
            {
                out.push_back(fmt[i]);
                continue;
            }
            if (i + 1 < fmt.size() && fmt[i + 1] == u'%')
            {
                out.push_back(u'%');
                i++;
                continue;
            }

            std::string spec = "%";
            size_t j = i + 1;
            auto take_number = [&] {
                if (j < fmt.size() && fmt[j] == u'*')
                {
                    spec += std::to_string(int32_t(args.next32()));
                    j++;
                    return;
                }
                while (j < fmt.size() && fmt[j] >= u'0' && fmt[j] <= u'9')
                    spec.push_back(char(fmt[j++]));
            };
            while (j < fmt.size() && std::u16string_view(u"-+ #0").find(fmt[j]) != std::u16string_view::npos)
                spec.push_back(char(fmt[j++]));
            take_number();
            if (j < fmt.size() && fmt[j] == u'.')
            {
                spec.push_back('.');
                j++;
                take_number();
            }

            // length: h, hh, l, ll, L, w, I64, I32, z
            int size = 32;
            bool wide_arg = false, narrow_arg = false;
            for (;;)
            {
                if (j >= fmt.size())
                    break;
                const char16_t c = fmt[j];
                if (c == u'l')
                {
                    if (j + 1 < fmt.size() && fmt[j + 1] == u'l')
                    {
                        size = 64;
                        j += 2;
                    }
                    else
                    {
                        wide_arg = true; // %ls / %lc
                        j++;
                    }
                }
                else if (c == u'h')
                {
                    narrow_arg = true;
                    j++;
                    if (j < fmt.size() && fmt[j] == u'h')
                        j++;
                }
                else if (c == u'w')
                {
                    wide_arg = true;
                    j++;
                }
                else if (c == u'L' || c == u'z' || c == u't' || c == u'j')
                    j++;
                else if (c == u'I' && j + 2 < fmt.size() && fmt[j + 1] == u'6' && fmt[j + 2] == u'4')
                {
                    size = 64;
                    j += 3;
                }
                else if (c == u'I' && j + 2 < fmt.size() && fmt[j + 1] == u'3' && fmt[j + 2] == u'2')
                    j += 3;
                else
                    break;
            }
            if (j >= fmt.size())
                break;
            const char16_t conv = fmt[j];
            i = j;

            char buf[512];
            buf[0] = 0;
            switch (conv)
            {
            case u'd':
            case u'i':
                spec += size == 64 ? "lld" : "d";
                if (size == 64)
                    snprintf(buf, sizeof(buf), spec.c_str(), (long long)args.next64());
                else
                    snprintf(buf, sizeof(buf), spec.c_str(), int32_t(args.next32()));
                break;
            case u'u':
            case u'x':
            case u'X':
            case u'o':
                spec += size == 64 ? "ll" : "";
                spec.push_back(char(conv));
                if (size == 64)
                    snprintf(buf, sizeof(buf), spec.c_str(), (unsigned long long)args.next64());
                else
                    snprintf(buf, sizeof(buf), spec.c_str(), args.next32());
                break;
            case u'p':
                snprintf(buf, sizeof(buf), "%08X", args.next32());
                break;
            case u'c':
            case u'C':
                out.push_back(char16_t(args.next32() & 0xFFFF));
                continue;
            case u'f':
            case u'F':
            case u'e':
            case u'E':
            case u'g':
            case u'G':
            case u'a':
            case u'A':
                spec.push_back(char(conv));
                snprintf(buf, sizeof(buf), spec.c_str(), args.next_double());
                break;
            case u's':
            case u'S':
            {
                // In wide printf %s is wide and %S narrow; in narrow printf the reverse.
                bool is_wide = (conv == u's') == wide_format;
                if (wide_arg)
                    is_wide = true;
                if (narrow_arg)
                    is_wide = false;
                const uint32_t p = args.next32();
                std::string s;
                if (!p)
                    s = "(null)";
                else if (is_wide)
                    s = narrow(read_wide(p));
                else
                    s = reinterpret_cast<const char*>(g_base + p);
                spec += "s";
                std::string padded(s.size() + 256, '\0');
                snprintf(padded.data(), padded.size(), spec.c_str(), s.c_str());
                for (const char* c = padded.c_str(); *c; c++)
                    out.push_back(char16_t(uint8_t(*c)));
                continue;
            }
            case u'n':
                args.next32(); // never write through %n
                continue;
            default:
                out.push_back(u'%');
                out.push_back(conv);
                continue;
            }
            for (const char* c = buf; *c; c++)
                out.push_back(char16_t(uint8_t(*c)));
        }
        return out;
    }

    std::u16string widen(const char* s)
    {
        std::u16string w;
        for (; *s; s++)
            w.push_back(char16_t(uint8_t(*s)));
        return w;
    }

    // Writes like _snprintf: at most `count` chars, NUL only if it fits; returns -1 when
    // truncated. count == ~0u means unbounded (sprintf).
    int32_t write_narrow(uint32_t dest, uint32_t count, const std::u16string& text)
    {
        const size_t n = text.size();
        const size_t limit = count == ~0u ? n + 1 : count;
        for (size_t i = 0; i < n && i < limit; i++)
            g_base[dest + i] = uint8_t(text[i]);
        if (n < limit)
            g_base[dest + n] = 0;
        return n > limit ? -1 : int32_t(n);
    }

    int32_t write_wide(uint32_t dest, uint32_t count, const std::u16string& text)
    {
        const size_t n = text.size();
        const size_t limit = count == ~0u ? n + 1 : count;
        for (size_t i = 0; i < n && i < limit; i++)
            store16(dest + uint32_t(i) * 2, uint16_t(text[i]));
        if (n < limit)
            store16(dest + uint32_t(n) * 2, 0);
        return n > limit ? -1 : int32_t(n);
    }

    std::u16string narrow_format(uint32_t fmt) { return widen(reinterpret_cast<const char*>(g_base + fmt)); }

    // FILETIME (100 ns since 1601) <-> calendar
    constexpr int64_t kEpochDelta = 116444736000000000ll;

    int64_t now_filetime()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() * 10 + kEpochDelta;
    }
}

// ---------------------------------------------------------------------- printf family

TT2_IMPORT(sprintf)
{
    RegisterArgs args(ctx, 2);
    ctx.r3.s64 = write_narrow(ctx.r3.u32, ~0u, format(narrow_format(ctx.r4.u32), args, false));
}

TT2_IMPORT(_snprintf)
{
    const uint32_t dest = ctx.r3.u32, count = ctx.r4.u32, fmt = ctx.r5.u32;
    RegisterArgs args(ctx, 3);
    ctx.r3.s64 = write_narrow(dest, count, format(narrow_format(fmt), args, false));
}

TT2_IMPORT(_vsnprintf)
{
    VaListArgs args(ctx.r6.u32);
    ctx.r3.s64 = write_narrow(ctx.r3.u32, ctx.r4.u32, format(narrow_format(ctx.r5.u32), args, false));
}

TT2_IMPORT(swprintf)
{
    RegisterArgs args(ctx, 2);
    ctx.r3.s64 = write_wide(ctx.r3.u32, ~0u, format(read_wide(ctx.r4.u32), args, true));
}

TT2_IMPORT(_snwprintf)
{
    const uint32_t dest = ctx.r3.u32, count = ctx.r4.u32, fmt = ctx.r5.u32;
    RegisterArgs args(ctx, 3);
    ctx.r3.s64 = write_wide(dest, count, format(read_wide(fmt), args, true));
}

TT2_IMPORT(vswprintf)
{
    VaListArgs args(ctx.r5.u32);
    ctx.r3.s64 = write_wide(ctx.r3.u32, ~0u, format(read_wide(ctx.r4.u32), args, true));
}

TT2_IMPORT(_vscwprintf)
{
    VaListArgs args(ctx.r4.u32);
    ctx.r3.s64 = int64_t(format(read_wide(ctx.r3.u32), args, true).size());
}

TT2_IMPORT(DbgPrint)
{
    RegisterArgs args(ctx, 1);
    log("guest: %s", narrow(format(narrow_format(ctx.r3.u32), args, false)).c_str());
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------- strings

// ANSI_STRING / UNICODE_STRING: be16 Length, be16 MaximumLength, be32 Buffer
TT2_IMPORT(RtlInitAnsiString)
{
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    const uint16_t len = src ? uint16_t(strlen(reinterpret_cast<const char*>(g_base + src))) : 0;
    store16(dst, len);
    store16(dst + 2, src ? uint16_t(len + 1) : 0);
    store32(dst + 4, src);
}

TT2_IMPORT(RtlInitUnicodeString)
{
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    const uint16_t len = uint16_t(read_wide(src).size() * 2);
    store16(dst, len);
    store16(dst + 2, src ? uint16_t(len + 2) : 0);
    store32(dst + 4, src);
}

TT2_IMPORT(RtlFreeAnsiString)
{
    const uint32_t s = ctx.r3.u32;
    if (const uint32_t buffer = load32(s + 4))
        g_memory.virt.free(buffer);
    store32(s + 4, 0);
    store16(s, 0);
    store16(s + 2, 0);
}

// RtlUnicodeStringToAnsiString(ANSI_STRING* dst, UNICODE_STRING* src, BOOLEAN allocate)
TT2_IMPORT(RtlUnicodeStringToAnsiString)
{
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    const uint16_t wlen = load16(src) / 2;
    std::u16string w;
    for (uint16_t i = 0; i < wlen; i++)
        w.push_back(char16_t(load16(load32(src + 4) + i * 2)));
    const std::string s = narrow(w);
    uint32_t buffer = load32(dst + 4);
    if (ctx.r5.u32)
    {
        buffer = g_memory.virt.alloc(uint32_t(s.size() + 1), 16);
        store32(dst + 4, buffer);
        store16(dst + 2, uint16_t(s.size() + 1));
    }
    else if (load16(dst + 2) < s.size() + 1)
    {
        ctx.r3.u64 = status::buffer_overflow;
        return;
    }
    std::memcpy(g_base + buffer, s.c_str(), s.size() + 1);
    store16(dst, uint16_t(s.size()));
    ctx.r3.u64 = status::success;
}

// RtlMultiByteToUnicodeN(wchar* dst, ULONG dst_bytes, ULONG* written, char* src, ULONG src_bytes)
TT2_IMPORT(RtlMultiByteToUnicodeN)
{
    Args a{ ctx };
    const uint32_t count = std::min(a.u32(1) / 2, a.u32(4));
    for (uint32_t i = 0; i < count; i++)
        store16(a.u32(0) + i * 2, g_base[a.u32(3) + i]);
    if (auto* w = a.ptr<be<uint32_t>>(2))
        *w = count * 2;
    a.ret(status::success);
}

// RtlUnicodeToMultiByteN(char* dst, ULONG dst_bytes, ULONG* written, wchar* src, ULONG src_bytes)
TT2_IMPORT(RtlUnicodeToMultiByteN)
{
    Args a{ ctx };
    const uint32_t count = std::min(a.u32(1), a.u32(4) / 2);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint16_t c = load16(a.u32(3) + i * 2);
        g_base[a.u32(0) + i] = c < 0x100 ? uint8_t(c) : '?';
    }
    if (auto* w = a.ptr<be<uint32_t>>(2))
        *w = count;
    a.ret(status::success);
}

TT2_IMPORT(RtlUpcaseUnicodeChar)
{
    const uint16_t c = uint16_t(ctx.r3.u32);
    ctx.r3.u64 = (c >= 'a' && c <= 'z') ? c - 32 : c;
}

// RtlCompareStringN(s1, len1, s2, len2, case_insensitive)
TT2_IMPORT(RtlCompareStringN)
{
    Args a{ ctx };
    const char* s1 = reinterpret_cast<const char*>(g_base + a.u32(0));
    const char* s2 = reinterpret_cast<const char*>(g_base + a.u32(2));
    const uint32_t n1 = a.u32(1), n2 = a.u32(3);
    const bool ci = a.u32(4) != 0;
    const uint32_t n = std::min(n1, n2);
    for (uint32_t i = 0; i < n; i++)
    {
        int c1 = uint8_t(s1[i]), c2 = uint8_t(s2[i]);
        if (ci)
        {
            c1 = std::toupper(c1);
            c2 = std::toupper(c2);
        }
        if (c1 != c2)
            return a.ret(uint64_t(int64_t(c1 - c2)));
    }
    a.ret(uint64_t(int64_t(int32_t(n1) - int32_t(n2))));
}

// ---------------------------------------------------------------------- time

TT2_IMPORT(KeQuerySystemTime)
{
    if (ctx.r3.u32)
        store64(ctx.r3.u32, uint64_t(now_filetime()));
}

TT2_IMPORT(KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = 50'000'000; // the Xbox 360 timebase; mftb is mapped to the same rate
}

// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (be16 each)
TT2_IMPORT(RtlTimeToTimeFields)
{
    const int64_t ft = int64_t(load64(ctx.r3.u32));
    const uint32_t out = ctx.r4.u32;
    const int64_t unix_ms = (ft - kEpochDelta) / 10000;
    const time_t secs = time_t(unix_ms / 1000);
    tm t{};
    gmtime_s(&t, &secs);
    store16(out + 0, uint16_t(t.tm_year + 1900));
    store16(out + 2, uint16_t(t.tm_mon + 1));
    store16(out + 4, uint16_t(t.tm_mday));
    store16(out + 6, uint16_t(t.tm_hour));
    store16(out + 8, uint16_t(t.tm_min));
    store16(out + 10, uint16_t(t.tm_sec));
    store16(out + 12, uint16_t(unix_ms % 1000));
    store16(out + 14, uint16_t(t.tm_wday));
}

TT2_IMPORT(RtlTimeFieldsToTime)
{
    const uint32_t in = ctx.r3.u32;
    tm t{};
    t.tm_year = load16(in) - 1900;
    t.tm_mon = load16(in + 2) - 1;
    t.tm_mday = load16(in + 4);
    t.tm_hour = load16(in + 6);
    t.tm_min = load16(in + 8);
    t.tm_sec = load16(in + 10);
    const int64_t secs = int64_t(_mkgmtime(&t));
    if (secs < 0)
    {
        ctx.r3.u64 = 0;
        return;
    }
    store64(ctx.r4.u32, uint64_t((secs * 1000 + load16(in + 12)) * 10000 + kEpochDelta));
    ctx.r3.u64 = 1;
}

// ---------------------------------------------------------------------- errors / exceptions

TT2_IMPORT(RtlNtStatusToDosError)
{
    const uint32_t s = ctx.r3.u32;
    uint32_t e = 317; // ERROR_MR_MID_NOT_FOUND
    switch (s)
    {
    case status::success: e = 0; break;
    case status::pending: e = win32::io_pending; break;
    case status::no_such_file:
    case status::object_name_not_found: e = win32::file_not_found; break;
    case status::object_path_not_found: e = 3; break;
    case status::access_denied: e = win32::access_denied; break;
    case status::invalid_handle: e = win32::invalid_handle; break;
    case status::end_of_file: e = 38; break;
    case status::no_more_files: e = win32::no_more_files; break;
    case status::no_memory: e = 8; break;
    case status::object_name_collision: e = 183; break;
    case status::timeout: e = 1460; break;
    default:
        if ((s & 0xFFFF0000) == 0x80070000)
            e = s & 0xFFFF;
        break;
    }
    ctx.r3.u64 = e;
}

TT2_IMPORT(RtlRaiseException)
{
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = record ? load32(record) : 0;
    if (code == 0x406D1388) // MSVC "set thread name": the name is at ExceptionInformation[1]
    {
        const uint32_t name = load32(record + 0x18);
        log("guest thread name: %s", name ? reinterpret_cast<const char*>(g_base + name) : "?");
        return;
    }
    log("guest raised exception %08X (not supported; continuing)", code);
}

TT2_IMPORT(RtlUnwind) { log("guest called RtlUnwind (exceptions are not supported)"); }
TT2_IMPORT(RtlCaptureContext) {}
TT2_IMPORT(__C_specific_handler) { ctx.r3.u64 = 1; } // ExceptionContinueSearch

// RtlImageXexHeaderField(xex_header, key): inline values are returned directly, other
// headers as a pointer to their data (copied from Default.xex by tt2-analyze).
TT2_IMPORT(RtlImageXexHeaderField)
{
    const auto it = g_xex_headers.find(ctx.r4.u32);
    if (it == g_xex_headers.end())
        log("RtlImageXexHeaderField(%08X) -> not present", ctx.r4.u32);
    ctx.r3.u64 = it == g_xex_headers.end() ? 0 : it->second;
}
