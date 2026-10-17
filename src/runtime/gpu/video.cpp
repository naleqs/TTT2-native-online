// Video imports and a "null" GPU.
//
// The game drives the Xenos GPU through the XDK's Direct3D library, which writes PM4 command
// packets into a ring buffer and waits on values the GPU writes back (fences, scratch
// registers, read pointer). Rendering is not implemented yet, so this command processor only
// executes what the CPU side waits for, the same subset Xenia's null graphics backend needs:
// register writes (with scratch writeback), MEM_WRITE, EVENT_WRITE_*, WAIT_REG_MEM, REG_RMW,
// COND_WRITE, INTERRUPT and indirect buffers. Packet semantics follow Xenia's
// command_processor.cc (BSD-3-Clause).

#include "video.h"
#include "../kernel/threads.h"
#include "../memory.h"
#include "../netplay_glue.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace tt2;
using namespace tt2::kernel;

namespace tt2::video
{
    namespace
    {
        constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
        constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
        constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A2F;

        std::atomic<uint32_t> g_ring_base{ 0 };  // physical
        std::atomic<uint32_t> g_ring_dwords{ 0 };
        std::atomic<uint32_t> g_rptr_writeback{ 0 };
        uint32_t g_rptr = 0;
        std::atomic<uint32_t> g_interrupt_callback{ 0 }, g_interrupt_data{ 0 };
        std::atomic<uint64_t> g_frames{ 0 };
        std::atomic<bool> g_running{ false };
        std::thread g_thread;
        uint32_t g_vblank_counter = 0;

        uint32_t guest_from_physical(uint32_t p)
        {
            return p >= 0x20000000 ? p : (p | 0xA0000000);
        }

        uint32_t reg(uint32_t index) { return load32(Memory::kGpuRegisters + index * 4); }
        void set_reg(uint32_t index, uint32_t v) { store32(Memory::kGpuRegisters + index * 4, v); }

        uint32_t gpu_swap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8); // 8 in 16
            case 2: return __builtin_bswap32(v);                               // 8 in 32
            case 3: return (v << 16) | (v >> 16);                              // 16 in 32
            default: return v;
            }
        }

        // Stores a value the way the GPU does: GpuSwap, then host order into memory.
        void gpu_store(uint32_t address_with_endian, uint32_t value)
        {
            const uint32_t addr = guest_from_physical(address_with_endian & ~3u);
            const uint32_t v = gpu_swap(value, address_with_endian & 3);
            std::memcpy(g_base + addr, &v, 4);
        }

        uint32_t gpu_load(uint32_t address_with_endian)
        {
            uint32_t v;
            std::memcpy(&v, g_base + guest_from_physical(address_with_endian & ~3u), 4);
            return gpu_swap(v, address_with_endian & 3);
        }

        void write_register(uint32_t index, uint32_t value)
        {
            set_reg(index, value);
            if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
            {
                const uint32_t n = index - REG_SCRATCH_REG0;
                if (reg(REG_SCRATCH_UMSK) & (1u << n))
                    store32(guest_from_physical(reg(REG_SCRATCH_ADDR) + n * 4), value);
            }
            else if (index == REG_COHER_STATUS_HOST && (value & 0x80000000))
            {
                set_reg(index, value & ~0x80000000u); // cache flush done
            }
        }

        void interrupt(uint32_t source)
        {
            if (const uint32_t cb = g_interrupt_callback)
                call_guest(cb, source, uint32_t(g_interrupt_data));
        }

        bool compare(uint32_t function, uint32_t value, uint32_t ref)
        {
            switch (function & 7)
            {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }

        struct Reader
        {
            uint32_t base;   // guest address of the buffer
            uint32_t size;   // dwords (ring size for the primary buffer)
            uint32_t pos;    // dword index
            uint32_t end;    // dword index to stop at
            uint32_t read()
            {
                const uint32_t v = load32(base + (pos % size) * 4);
                pos++;
                return v;
            }
            bool done() const { return pos == end; }
        };

        void execute(Reader& r, int depth);

        void execute_type3(Reader& r, uint32_t header, int depth)
        {
            const uint32_t opcode = (header >> 8) & 0x7F;
            const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            const uint32_t next = r.pos + count;

            switch (opcode)
            {
            case 0x3F: // INDIRECT_BUFFER
            case 0x37: // INDIRECT_BUFFER_PFD
            {
                const uint32_t addr = r.read();
                const uint32_t dwords = r.read() & 0xFFFFF;
                if (depth < 4 && dwords)
                {
                    Reader ib{ guest_from_physical(addr), dwords, 0, dwords };
                    execute(ib, depth + 1);
                }
                break;
            }
            case 0x3D: // MEM_WRITE: address, then values
            {
                const uint32_t addr = r.read();
                for (uint32_t i = 1; i < count; i++)
                    gpu_store((addr & ~3u) + (i - 1) * 4 | (addr & 3), r.read());
                break;
            }
            case 0x58: // EVENT_WRITE_SHD: initiator, address, value
            {
                const uint32_t initiator = r.read();
                const uint32_t addr = r.read();
                const uint32_t value = r.read();
                gpu_store(addr, (initiator >> 31) ? g_vblank_counter : value);
                break;
            }
            case 0x59: // EVENT_WRITE_EXT: initiator, address -> screen extents
            {
                r.read();
                const uint32_t addr = r.read();
                const uint16_t extents[6] = { 0, 0x2000, 0, 0x2000, 0, 0x2000 };
                for (int i = 0; i < 3; i++)
                    gpu_store((addr & ~3u) + i * 4 | (addr & 3), uint32_t(extents[i * 2]) << 16 | extents[i * 2 + 1]);
                break;
            }
            case 0x3C: // WAIT_REG_MEM: info, poll address/register, reference, mask, wait
            {
                const uint32_t info = r.read();
                const uint32_t poll = r.read();
                const uint32_t ref = r.read();
                const uint32_t mask = r.read();
                r.read();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                for (;;)
                {
                    const uint32_t v = ((info & 0x10) ? gpu_load(poll) : reg(poll & 0xFFFF)) & mask;
                    if (compare(info, v, ref) || !g_running)
                        break;
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        log("gpu: WAIT_REG_MEM on %08X timed out", poll);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                break;
            }
            case 0x21: // REG_RMW: register, and-mask, or-mask
            {
                const uint32_t info = r.read();
                const uint32_t and_mask = r.read();
                const uint32_t or_mask = r.read();
                const uint32_t index = info & 0x1FFF;
                uint32_t v = reg(index);
                v &= (info & 0x80000000) ? reg(and_mask & 0x1FFF) : and_mask;
                v |= (info & 0x40000000) ? reg(or_mask & 0x1FFF) : or_mask;
                write_register(index, v);
                break;
            }
            case 0x45: // COND_WRITE: info, poll, ref, mask, write address/register, value
            {
                const uint32_t info = r.read();
                const uint32_t poll = r.read();
                const uint32_t ref = r.read();
                const uint32_t mask = r.read();
                const uint32_t write_addr = r.read();
                const uint32_t write_value = r.read();
                const uint32_t v = ((info & 0x10) ? gpu_load(poll) : reg(poll & 0xFFFF)) & mask;
                if (compare(info, v, ref))
                {
                    if (info & 0x100)
                        gpu_store(write_addr, write_value);
                    else
                        write_register(write_addr & 0xFFFF, write_value);
                }
                break;
            }
            case 0x54: // INTERRUPT
                r.read();
                interrupt(1);
                break;
            default:
                break; // draws, constants, state: rendering is not implemented
            }
            r.pos = next;
        }

        void execute(Reader& r, int depth)
        {
            while (!r.done() && g_running)
            {
                const uint32_t header = r.read();
                switch (header >> 30)
                {
                case 0: // register writes
                {
                    const uint32_t base = header & 0x7FFF;
                    const bool one_reg = (header >> 15) & 1;
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    for (uint32_t i = 0; i < count; i++)
                        write_register(one_reg ? base : base + i, r.read());
                    break;
                }
                case 1: // two register writes
                {
                    const uint32_t a = r.read(), b = r.read();
                    write_register(header & 0x7FF, a);
                    write_register((header >> 11) & 0x7FF, b);
                    break;
                }
                case 2: // NOP
                    break;
                default:
                    execute_type3(r, header, depth);
                    break;
                }
            }
        }

        void gpu_thread()
        {
            adopt_host_thread("gpu");
            auto next_vblank = std::chrono::steady_clock::now();
            while (g_running)
            {
                const uint32_t ring = g_ring_base, dwords = g_ring_dwords;
                if (ring && dwords)
                {
                    const uint32_t wptr = reg(REG_CP_RB_WPTR) % dwords;
                    if (wptr != g_rptr)
                    {
                        Reader r{ guest_from_physical(ring), dwords, g_rptr, wptr < g_rptr ? wptr + dwords : wptr };
                        execute(r, 0);
                        g_rptr = wptr;
                        if (const uint32_t wb = g_rptr_writeback)
                            store32(guest_from_physical(wb), g_rptr);
                    }
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= next_vblank)
                {
                    next_vblank += std::chrono::microseconds(16667);
                    if (now - next_vblank > std::chrono::milliseconds(100))
                        next_vblank = now;
                    g_vblank_counter++;
                    interrupt(0);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        }
    }

    void start()
    {
        if (g_running.exchange(true))
            return;
        g_thread = std::thread(gpu_thread);
    }

    void stop()
    {
        if (!g_running.exchange(false))
            return;
        if (g_thread.joinable())
            g_thread.join();
    }

    uint64_t frames() { return g_frames; }
}

using namespace tt2::video;

TT2_IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
TT2_IMPORT(VdShutdownEngines) {}

TT2_IMPORT(VdSetGraphicsInterruptCallback)
{
    g_interrupt_callback = ctx.r3.u32;
    g_interrupt_data = ctx.r4.u32;
}

// VdInitializeRingBuffer(physical address, log2 of size in qwords)
TT2_IMPORT(VdInitializeRingBuffer)
{
    g_rptr = 0;
    g_ring_dwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_ring_base = ctx.r3.u32;
    start();
}

TT2_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    g_rptr_writeback = ctx.r3.u32;
}

TT2_IMPORT(VdGetSystemCommandBuffer)
{
    static uint32_t buffer = 0;
    if (!buffer)
        buffer = g_memory.physical.alloc(0x10000, 0x1000);
    if (ctx.r3.u32)
        store32(ctx.r3.u32, buffer);
    if (ctx.r4.u32)
        store32(ctx.r4.u32, 0xBEEF0000);
}

TT2_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}
TT2_IMPORT(VdInitializeScalerCommandBuffer) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 0x7; } // widescreen, 720p, 1080i capable

TT2_IMPORT(VdQueryVideoMode)
{
    const uint32_t m = ctx.r3.u32;
    std::memset(g_base + m, 0, 48);
    store32(m + 0, 1280);
    store32(m + 4, 720);
    store32(m + 12, 1);
    store32(m + 16, 1);
    store32(m + 20, std::bit_cast<uint32_t>(60.0f));
    store32(m + 24, 1);
}

TT2_IMPORT(VdGetCurrentDisplayInformation)
{
    const uint32_t info = ctx.r3.u32;
    std::memset(g_base + info, 0, 0x58);
    store16(info + 0, 1280); // front buffer width
    store16(info + 2, 720);  // front buffer height
    store32(info + 0x10, 1280);
    store32(info + 0x14, 720);
}

TT2_IMPORT(VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32)
        store32(ctx.r3.u32, 1);
    if (ctx.r4.u32)
        store32(ctx.r4.u32, std::bit_cast<uint32_t>(2.22222233f));
}

TT2_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdSetDisplayModeOverride) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdPersistDisplay) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
TT2_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
TT2_IMPORT(VdEnableDisableClockGating) {}
TT2_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// VdSwap(buffer, fetch_constant, ...): the library reserved 64 dwords in the ring for the
// swap packet. Fill them with one NOP packet and present on the host.
TT2_IMPORT(VdSwap)
{
    if (const uint32_t buffer = ctx.r3.u32)
    {
        store32(buffer, (3u << 30) | (62u << 16) | (0x10u << 8)); // type 3 NOP, 63 dwords follow
        for (uint32_t i = 1; i < 64; i++)
            store32(buffer + i * 4, 0);
    }
    g_frames++;
    netglue::on_present();
    ctx.r3.u64 = 0;
}
