#include "memory.h"
#include "guest.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace tt2
{
    uint8_t* g_base = nullptr;
    Memory g_memory;

    RangeAllocator::RangeAllocator(uint32_t begin, uint32_t end, uint32_t granularity)
        : begin_(begin), end_(end), granularity_(granularity)
    {
        free_[begin] = end - begin;
    }

    uint32_t RangeAllocator::alloc(uint32_t size, uint32_t alignment, bool top_down)
    {
        if (size == 0)
            size = 1;
        size = round(size);
        alignment = std::max(alignment, granularity_);
        std::lock_guard lock(mutex_);

        auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t start) {
            const uint32_t block = it->first, block_size = it->second;
            free_.erase(it);
            if (start > block)
                free_[block] = start - block;
            const uint64_t block_end = uint64_t(block) + block_size;
            if (uint64_t(start) + size < block_end)
                free_[start + size] = uint32_t(block_end - start - size);
            used_[start] = size;
            return start;
        };

        if (top_down)
        {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it)
            {
                const uint64_t block_end = uint64_t(it->first) + it->second;
                if (block_end < size)
                    continue;
                const uint32_t start = uint32_t((block_end - size) & ~uint64_t(alignment - 1));
                if (start >= it->first)
                    return take(std::prev(it.base()), start);
            }
            return 0;
        }

        for (auto it = free_.begin(); it != free_.end(); ++it)
        {
            const uint32_t start = (it->first + alignment - 1) & ~(alignment - 1);
            if (start >= it->first && uint64_t(start) + size <= uint64_t(it->first) + it->second)
                return take(it, start);
        }
        return 0;
    }

    bool RangeAllocator::alloc_at(uint32_t address, uint32_t size)
    {
        size = round(size);
        address &= ~(granularity_ - 1);
        std::lock_guard lock(mutex_);
        auto it = free_.upper_bound(address);
        if (it == free_.begin())
            return false;
        --it;
        const uint64_t block_end = uint64_t(it->first) + it->second;
        if (uint64_t(address) + size > block_end)
            return false;
        const uint32_t block = it->first;
        free_.erase(it);
        if (address > block)
            free_[block] = address - block;
        if (uint64_t(address) + size < block_end)
            free_[address + size] = uint32_t(block_end - address - size);
        used_[address] = size;
        return true;
    }

    void RangeAllocator::free(uint32_t address)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.find(address);
        if (it == used_.end())
            return;
        uint32_t start = it->first, size = it->second;
        used_.erase(it);

        // merge with neighbours
        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + size)
        {
            size += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (uint64_t(prev->first) + prev->second == start)
            {
                start = prev->first;
                size += prev->second;
                free_.erase(prev);
            }
        }
        free_[start] = size;
    }

    uint32_t RangeAllocator::size_of(uint32_t address) const
    {
        std::lock_guard lock(mutex_);
        auto it = used_.find(address);
        return it == used_.end() ? 0 : it->second;
    }

    bool RangeAllocator::find(uint32_t address, uint32_t& start, uint32_t& size) const
    {
        std::lock_guard lock(mutex_);
        auto it = used_.upper_bound(address);
        if (it == used_.begin())
            return false;
        --it;
        if (address >= uint64_t(it->first) + it->second)
            return false;
        start = it->first;
        size = it->second;
        return true;
    }

    uint64_t RangeAllocator::used() const
    {
        std::lock_guard lock(mutex_);
        uint64_t total = 0;
        for (auto& [s, n] : used_)
            total += n;
        return total;
    }

    bool Memory::init()
    {
        // Prefer a fixed host address so guest pointers look the same in every crash log.
        constexpr DWORD kFlags = MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH;
        g_base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x100000000ull), PPC_MEMORY_SIZE, kFlags, PAGE_READWRITE));
        if (!g_base)
            g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, kFlags, PAGE_READWRITE));
        if (!g_base)
            return false;

        // Guest null pointer dereferences fault instead of reading zeros.
        DWORD old;
        VirtualProtect(g_base, 0x10000, PAGE_NOACCESS, &old);
        return true;
    }

    void Memory::fill_function_table()
    {
        for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
            if (PPCFuncMappings[i].host)
                PPC_LOOKUP_FUNC(g_base, PPCFuncMappings[i].guest) = PPCFuncMappings[i].host;
    }

    uint32_t Memory::system_alloc(uint32_t size, uint32_t alignment)
    {
        const uint32_t a = system.alloc(size, alignment);
        if (!a)
            fatal("out of runtime system memory (%u bytes)", size);
        std::memset(g_base + a, 0, size);
        return a;
    }

    void Memory::system_free(uint32_t address)
    {
        system.free(address);
    }
}
