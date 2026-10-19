#pragma once

// Guest address space: one 4 GB reservation at g_base, committed up front and write-watched
// so the rollback snapshots can see which pages each frame touched.
//
// Xbox 360 layout used here:
//   0x40000000-0x7DFFFFFF  virtual allocations (NtAllocateVirtualMemory, pools, stacks)
//   0x7E000000-0x7EFFFFFF  runtime-owned kernel structures (PCR/TLS/TEB, guest objects)
//   0x7FC80000-0x7FCFFFFF  GPU registers (MMIO stores land here; the null GPU reads them)
//   0x82000000-...         the game image, then XenonRecomp's function lookup table
//   0xA0000000-0xBFFFFFFF  physical allocations (MmAllocatePhysicalMemoryEx), seen by the GPU

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace tt2
{
    // First-fit range allocator over a guest address range (page granularity).
    class RangeAllocator
    {
    public:
        RangeAllocator(uint32_t begin, uint32_t end, uint32_t granularity);

        // Returns 0 when out of space. `alignment` is a power of two >= granularity.
        uint32_t alloc(uint32_t size, uint32_t alignment = 0, bool top_down = false);
        // Reserves a caller-chosen range (NtAllocateVirtualMemory with a base address).
        bool alloc_at(uint32_t address, uint32_t size);
        void free(uint32_t address);
        uint32_t size_of(uint32_t address) const; // 0 if not an allocation start
        bool contains(uint32_t address) const { return address >= begin_ && address < end_; }
        // Allocation containing `address`: start and size (false if none)
        bool find(uint32_t address, uint32_t& start, uint32_t& size) const;
        uint64_t used() const;
        uint32_t begin() const { return begin_; }
        uint32_t end() const { return end_; }
        // Live allocations (start, size).
        std::vector<std::pair<uint32_t, uint32_t>> ranges() const;
        // Called (under the allocator lock) after every successful allocation; rollback
        // tracks new memory with it. Set it with set_on_alloc: other threads allocate meanwhile.
        void set_on_alloc(std::function<void(uint32_t start, uint32_t size)> fn);

    private:
        std::function<void(uint32_t start, uint32_t size)> on_alloc;

    public:

    private:
        uint32_t round(uint32_t v) const { return (v + granularity_ - 1) & ~(granularity_ - 1); }

        uint32_t begin_, end_, granularity_;
        mutable std::mutex mutex_;
        std::map<uint32_t, uint32_t> free_;  // start -> size
        std::map<uint32_t, uint32_t> used_;  // start -> size
    };

    struct Memory
    {
        static constexpr uint32_t kVirtualBegin = 0x40000000;
        static constexpr uint32_t kVirtualEnd = 0x7E000000;
        static constexpr uint32_t kSystemBegin = 0x7E000000;
        static constexpr uint32_t kSystemEnd = 0x7F000000;
        static constexpr uint32_t kGpuRegisters = 0x7FC80000;
        static constexpr uint32_t kPhysicalBegin = 0xA0000000;
        static constexpr uint32_t kPhysicalEnd = 0xC0000000;

        RangeAllocator virt{ kVirtualBegin, kVirtualEnd, 0x1000 };
        RangeAllocator system{ kSystemBegin, kSystemEnd, 0x10 };
        RangeAllocator physical{ kPhysicalBegin, kPhysicalEnd, 0x1000 };

        bool init();
        void fill_function_table();

        // Zeroed allocation in the runtime's system range (kernel structures, guest objects).
        uint32_t system_alloc(uint32_t size, uint32_t alignment = 16);
        void system_free(uint32_t address);
    };

    extern Memory g_memory;
}
