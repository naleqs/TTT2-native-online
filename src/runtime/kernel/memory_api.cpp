// Guest memory management imports.

#include "../guest.h"
#include "../memory.h"

using namespace tt2;

namespace
{
    constexpr uint32_t MEM_COMMIT = 0x1000;
    constexpr uint32_t MEM_RESERVE = 0x2000;
    constexpr uint32_t MEM_DECOMMIT = 0x4000;
    constexpr uint32_t MEM_RELEASE = 0x8000;
    constexpr uint32_t MEM_FREE = 0x10000;
    constexpr uint32_t MEM_PRIVATE = 0x20000;
    constexpr uint32_t MEM_TOP_DOWN = 0x100000;
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t MEM_16MB_PAGES = 0x80000000;
    constexpr uint32_t PAGE_READWRITE = 0x04;

    uint32_t page_size_for(uint32_t flags)
    {
        if (flags & MEM_16MB_PAGES)
            return 16u << 20;
        if (flags & MEM_LARGE_PAGES)
            return 64u << 10;
        return 4u << 10;
    }

    RangeAllocator* allocator_for(uint32_t address)
    {
        if (g_memory.virt.contains(address))
            return &g_memory.virt;
        if (g_memory.physical.contains(address))
            return &g_memory.physical;
        if (g_memory.system.contains(address))
            return &g_memory.system;
        return nullptr;
    }
}

// NtAllocateVirtualMemory(base*, size*, allocation_type, protect, debug)
TT2_IMPORT(NtAllocateVirtualMemory)
{
    Args a{ ctx };
    auto* base_ptr = a.ptr<be<uint32_t>>(0);
    auto* size_ptr = a.ptr<be<uint32_t>>(1);
    const uint32_t type = a.u32(2);
    if (!base_ptr || !size_ptr)
        return a.ret(status::invalid_parameter);

    const uint32_t page = page_size_for(type);
    const uint32_t size = (uint32_t(*size_ptr) + page - 1) & ~(page - 1);
    uint32_t address = *base_ptr;

    if (address)
    {
        // Committing inside an existing reservation needs no work: host memory is committed.
        uint32_t start, existing;
        if (g_memory.virt.find(address, start, existing))
        {
            if (type & MEM_COMMIT)
                std::memset(g_base + (address & ~(page - 1)), 0, std::min(size, start + existing - (address & ~(page - 1))));
            *base_ptr = address & ~(page - 1);
            *size_ptr = size;
            return a.ret(status::success);
        }
        if (!g_memory.virt.alloc_at(address, size))
            return a.ret(status::no_memory);
        address &= ~(page - 1);
    }
    else
    {
        address = g_memory.virt.alloc(size, page, (type & MEM_TOP_DOWN) != 0);
        if (!address)
        {
            log("memory: NtAllocateVirtualMemory of %u KB failed", size / 1024);
            return a.ret(status::no_memory);
        }
    }

    std::memset(g_base + address, 0, size);
    *base_ptr = address;
    *size_ptr = size;
    a.ret(status::success);
}

// NtFreeVirtualMemory(base*, size*, free_type, debug)
TT2_IMPORT(NtFreeVirtualMemory)
{
    Args a{ ctx };
    auto* base_ptr = a.ptr<be<uint32_t>>(0);
    if (!base_ptr)
        return a.ret(status::invalid_parameter);
    const uint32_t type = a.u32(2);
    if (type & MEM_RELEASE)
    {
        uint32_t start, size;
        if (!g_memory.virt.find(*base_ptr, start, size))
            return a.ret(status::invalid_parameter);
        g_memory.virt.free(start);
        if (auto* size_ptr = a.ptr<be<uint32_t>>(1))
            *size_ptr = size;
    }
    a.ret(status::success); // MEM_DECOMMIT: nothing to do
}

// NtQueryVirtualMemory(base, MEMORY_BASIC_INFORMATION* out, debug)
TT2_IMPORT(NtQueryVirtualMemory)
{
    Args a{ ctx };
    const uint32_t address = a.u32(0);
    auto* info = a.ptr<be<uint32_t>>(1);
    if (!info)
        return a.ret(status::invalid_parameter);

    uint32_t start = address & ~0xFFFu, size = 0x1000;
    bool used = false;
    if (auto* alloc = allocator_for(address))
        used = alloc->find(address, start, size);
    info[0] = address & ~0xFFFu;                  // BaseAddress
    info[1] = used ? start : 0;                   // AllocationBase
    info[2] = used ? PAGE_READWRITE : 0;          // AllocationProtect
    info[3] = used ? start + size - (address & ~0xFFFu) : 0x1000; // RegionSize
    info[4] = used ? MEM_COMMIT : MEM_FREE;       // State
    info[5] = used ? PAGE_READWRITE : 0;          // Protect
    info[6] = used ? MEM_PRIVATE : 0;             // Type
    a.ret(status::success);
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min_address, max_address, alignment)
TT2_IMPORT(MmAllocatePhysicalMemoryEx)
{
    Args a{ ctx };
    const uint32_t size = a.u32(1);
    const uint32_t protect = a.u32(2);
    const uint32_t page = page_size_for(protect);
    const uint32_t alignment = std::max(page, a.u32(5));
    const uint32_t address = g_memory.physical.alloc((size + page - 1) & ~(page - 1), alignment);
    if (!address)
        log("memory: MmAllocatePhysicalMemoryEx of %u KB failed", size / 1024);
    else
        std::memset(g_base + address, 0, size);
    a.ret(address);
}

TT2_IMPORT(MmFreePhysicalMemory)
{
    Args a{ ctx };
    g_memory.physical.free(a.u32(1));
}

TT2_IMPORT(MmGetPhysicalAddress)
{
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF;
}

TT2_IMPORT(MmMapIoSpace)
{
    ctx.r3.u64 = ctx.r3.u32;
}

TT2_IMPORT(MmQueryAddressProtect)
{
    ctx.r3.u64 = allocator_for(ctx.r3.u32) || (ctx.r3.u32 >= 0x82000000 && ctx.r3.u32 < 0x90000000) ? PAGE_READWRITE : 0;
}

TT2_IMPORT(MmQueryAllocationSize)
{
    uint32_t start, size;
    auto* alloc = allocator_for(ctx.r3.u32);
    ctx.r3.u64 = alloc && alloc->find(ctx.r3.u32, start, size) ? size : 0;
}

// MM_STATISTICS: Length, TotalPhysicalPages, KernelPages, TitleAvailablePages,
// TitleTotalVirtualMemoryBytes, TitleReservedVirtualMemoryBytes, TitlePhysicalPages, ...
TT2_IMPORT(MmQueryStatistics)
{
    Args a{ ctx };
    auto* s = a.ptr<be<uint32_t>>(0);
    if (!s)
        return a.ret(status::invalid_parameter);
    const uint32_t length = s[0];
    std::memset(s, 0, std::min<uint32_t>(length, 0x68));
    s[0] = length;
    const uint32_t total_pages = (512u << 20) / 4096;
    const uint32_t used_pages = uint32_t((g_memory.physical.used() + g_memory.virt.used()) / 4096);
    s[1] = total_pages;
    s[2] = 0x1000;                                       // kernel pages
    s[3] = total_pages - std::min(total_pages, used_pages + 0x1000); // title available pages
    s[4] = g_memory.kVirtualEnd - g_memory.kVirtualBegin; // title total virtual bytes
    s[5] = uint32_t(g_memory.virt.used());
    s[6] = used_pages;
    a.ret(status::success);
}

TT2_IMPORT(ExAllocatePool)
{
    const uint32_t size = ctx.r3.u32;
    const uint32_t address = g_memory.virt.alloc(size, 16);
    if (address)
        std::memset(g_base + address, 0, size);
    ctx.r3.u64 = address;
}

TT2_IMPORT(ExAllocatePoolTypeWithTag)
{
    const uint32_t size = ctx.r3.u32;
    const uint32_t address = g_memory.virt.alloc(size, 16);
    if (address)
        std::memset(g_base + address, 0, size);
    ctx.r3.u64 = address;
}

TT2_IMPORT(ExFreePool) { g_memory.virt.free(ctx.r3.u32); }

// XamAlloc(flags, size, out*)
TT2_IMPORT(XamAlloc)
{
    Args a{ ctx };
    const uint32_t size = a.u32(1);
    const uint32_t address = g_memory.virt.alloc(size, 16);
    if (address)
        std::memset(g_base + address, 0, size);
    if (auto* out = a.ptr<be<uint32_t>>(2))
        *out = address;
    a.ret(address ? 0 : 0x8007000E); // E_OUTOFMEMORY
}

TT2_IMPORT(XamFree)
{
    g_memory.virt.free(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

TT2_IMPORT(MmLockAndMapSegmentArray) { ctx.r3.u64 = status::success; }
TT2_IMPORT(MmUnlockAndUnmapSegmentArray) {}

TT2_IMPORT(RtlFillMemoryUlong)
{
    Args a{ ctx };
    const uint32_t dest = a.u32(0), length = a.u32(1), pattern = a.u32(2);
    for (uint32_t i = 0; i + 4 <= length; i += 4)
        store32(dest + i, pattern);
}

TT2_IMPORT(RtlCompareMemoryUlong)
{
    Args a{ ctx };
    const uint32_t src = a.u32(0), length = a.u32(1), pattern = a.u32(2);
    uint32_t matched = 0;
    while (matched + 4 <= length && load32(src + matched) == pattern)
        matched += 4;
    a.ret(matched);
}
