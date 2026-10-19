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
#include "../rollback/game_rollback.h"
#include "capture.h"
#include "renderer.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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
        constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t kSwapMarker = 0x53574150; // 'SWAP' in the NOP that VdSwap writes

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

        // The callback reads the "current processor" from the PCR: the CP interrupt handler clears
        // that processor's bit in a word the command stream then waits on, so it is delivered once
        // per processor in the packet's mask, as if it ran there.
        void interrupt(uint32_t source, uint32_t cpu_mask = 1)
        {
            const uint32_t cb = g_interrupt_callback;
            const auto self = kernel::current_thread();
            if (!cb || !self)
                return;
            uint8_t& processor = g_base[self->pcr + 0x10C];
            const uint8_t saved = processor;
            for (uint32_t cpu = 0; cpu < 6; cpu++)
                if (cpu_mask & (1u << cpu))
                {
                    processor = uint8_t(cpu);
                    call_guest(cb, source, uint32_t(g_interrupt_data));
                }
            processor = saved;
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

        // Vertical blank, 60 Hz. Raised from the GPU thread, also while the command processor
        // waits: the swap path waits for the vblank handler to retire the previous flip.
        std::chrono::steady_clock::time_point g_next_vblank;
        void service_vblank()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < g_next_vblank)
                return;
            g_next_vblank += std::chrono::microseconds(16667);
            if (now - g_next_vblank > std::chrono::milliseconds(100))
                g_next_vblank = now;
            g_vblank_counter++;
            interrupt(0);
        }

        // Waits are bounded so a value only a real GPU pipeline would produce cannot hang the game.
        constexpr auto kWaitTimeout = std::chrono::milliseconds(100);

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
            case 0x59: // EVENT_WRITE_CFL
                break;
            case 0x5A: // EVENT_WRITE_EXT: initiator, address -> screen extents
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
                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                for (;;)
                {
                    const uint32_t v = ((info & 0x10) ? gpu_load(poll) : reg(poll & 0xFFFF)) & mask;
                    if (compare(info, v, ref) || !g_running)
                        break;
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        static uint32_t timeouts = 0;
                        if (++timeouts <= 20 || timeouts % 1000 == 0)
                            log("gpu: WAIT_REG_MEM #%u on %08X timed out (info %08X, value %08X, mask %08X, ref %08X)", timeouts, poll, info,
                            (info & 0x10) ? gpu_load(poll) : reg(poll & 0xFFFF), mask, ref);
                        break;
                    }
                    service_vblank();
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
                interrupt(1, r.read());
                break;
            case 0x10: // NOP; VdSwap's marks the end of a frame
                if (count >= 5 && r.read() == kSwapMarker)
                {
                    const uint32_t frontbuffer = r.read(), width = r.read(), height = r.read();
                    gpu::on_frame_end();
                    gpu::renderer::swap(frontbuffer, width, height);
                }
                break;
            case 0x2D: // SET_CONSTANT: offset_type, values
            {
                const uint32_t offset_type = r.read();
                const uint32_t index = offset_type & 0x7FF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offset_type >> 16) & 0xFF;
                if (type < 5)
                    for (uint32_t i = 1; i < count; i++)
                        write_register(kBases[type] + index + i - 1, r.read());
                break;
            }
            case 0x55: // SET_CONSTANT2
            case 0x56: // SET_SHADER_CONSTANTS
            {
                const uint32_t index = r.read() & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    write_register(index + i - 1, r.read());
                break;
            }
            case 0x2F: // LOAD_ALU_CONSTANT: address, offset_type, size
            {
                const uint32_t address = guest_from_physical(r.read() & 0x3FFFFFFF);
                const uint32_t offset_type = r.read();
                const uint32_t size = r.read() & 0xFFF;
                static constexpr uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offset_type >> 16) & 0xFF;
                if (type < 5)
                    for (uint32_t i = 0; i < size; i++)
                        write_register(kBases[type] + (offset_type & 0x7FF) + i, load32(address + i * 4));
                break;
            }
            case 0x3E: // REG_TO_MEM: register, address
            {
                const uint32_t index = r.read() & 0xFFFF;
                const uint32_t address = r.read();
                gpu_store(address, reg(index));
                break;
            }
            case 0x4F: // MEM_WRITE_CNTR: address
                gpu_store(r.read(), g_vblank_counter);
                break;
            case 0x52: // WAIT_REG_EQ: register, value
            case 0x53: // WAIT_REG_GTE
            {
                const uint32_t index = r.read() & 0xFFFF;
                const uint32_t value = r.read();
                const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
                while (g_running && std::chrono::steady_clock::now() < deadline &&
                       !(opcode == 0x52 ? reg(index) == value : reg(index) >= value))
                {
                    service_vblank();
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
                break;
            }
            case 0x27: // IM_LOAD: address | type, start << 16 | size
            {
                const uint32_t addr_type = r.read();
                const uint32_t size = r.read() & 0xFFFF;
                const uint32_t address = guest_from_physical(addr_type & ~3u);
                gpu::on_shader_load(addr_type & 3, reinterpret_cast<const uint32_t*>(g_base + address), size);
                gpu::renderer::load_shader(addr_type & 3, reinterpret_cast<const uint32_t*>(g_base + address), size);
                break;
            }
            case 0x2B: // IM_LOAD_IMMEDIATE: type, start << 16 | size, words
            {
                const uint32_t type = r.read();
                const uint32_t size = r.read() & 0xFFFF;
                std::vector<uint32_t> words;
                for (uint32_t i = 0; i < size && i + 2 < count; i++)
                    words.push_back(__builtin_bswap32(r.read()));
                gpu::on_shader_load(type & 3, words.data(), uint32_t(words.size()));
                gpu::renderer::load_shader(type & 3, words.data(), uint32_t(words.size()));
                break;
            }
            case 0x22: // DRAW_INDX: viz token, initiator, [index base, index size]
            {
                r.read();
                const uint32_t initiator = r.read();
                uint32_t index_base = 0, index_size = 0;
                if (((initiator >> 6) & 3) == 0 && count >= 4)
                {
                    index_base = r.read();
                    index_size = r.read();
                }
                gpu::on_draw(initiator);
                gpu::renderer::draw(initiator, index_base, index_size);
                break;
            }
            case 0x36: // DRAW_INDX_2: initiator, [immediate indices]
            {
                const uint32_t initiator = r.read();
                gpu::on_draw(initiator);
                gpu::renderer::draw(initiator, 0, 0);
                break;
            }
            default:
                break; // other state: not needed without a renderer
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
            g_next_vblank = std::chrono::steady_clock::now();
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

                service_vblank();
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        }
    }

    void start()
    {
        if (g_running.exchange(true))
            return;
        gpu::configure();
        // Registers the D3D library reads instead of writes (values from Xenia's
        // GraphicsSystem::ReadRegister). The interrupt callback checks the interrupt status to
        // tell a vblank apart from other sources; with 0 it ignores every interrupt.
        set_reg(0x0F00, 0x08100748); // RB_EDRAM_TIMING
        set_reg(0x0F01, 0x0000200E); // RB_BC_CONTROL
        set_reg(0x194C, 0x000002D0); // D1MODE_V_COUNTER
        set_reg(0x1951, 0x00000001); // interrupt status: vblank
        set_reg(0x1961, 0x050002D0); // D1MODE_VIEWPORT_SIZE (1280x720)
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

    std::pair<uint32_t, uint32_t> ring_range()
    {
        const uint32_t ring = g_ring_base;
        return ring ? std::make_pair(guest_from_physical(ring), uint32_t(g_ring_dwords) * 4) : std::make_pair(0u, 0u);
    }
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

// VdGetSystemCommandBuffer(p0, p1): p0 is a 0x94-byte block filled in place (as Xenia does).
TT2_IMPORT(VdGetSystemCommandBuffer)
{
    if (const uint32_t p0 = ctx.r3.u32)
    {
        std::memset(g_base + p0, 0, 0x94);
        store32(p0, 0xBEEF0000);
    }
    if (ctx.r4.u32)
        store32(ctx.r4.u32, 0xBEEF0001);
}

TT2_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// VdInitializeScalerCommandBuffer(..., dest_ptr (arg 10), dest_count (arg 11)): the library runs
// this buffer on the GPU, so it must hold valid packets: fill it with NOPs, return its length.
TT2_IMPORT(VdInitializeScalerCommandBuffer)
{
    Args a{ ctx };
    const uint32_t dest = a.u32(10), count = a.u32(11);
    for (uint32_t i = 0; dest && i < count; i++)
        store32(dest + i * 4, 0x80000000);
    a.ret(count);
}
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
        store32(buffer + 4, kSwapMarker);                           // end of frame, for the GPU thread
        // The front buffer, from its texture fetch constant (second argument).
        uint32_t frontbuffer = 0, width = 0, height = 0;
        if (const uint32_t fetch = ctx.r4.u32)
        {
            frontbuffer = load32(fetch + 4) & 0xFFFFF000;
            const uint32_t size = load32(fetch + 8);
            width = (size & 0x1FFF) + 1;
            height = ((size >> 13) & 0x1FFF) + 1;
        }
        store32(buffer + 8, frontbuffer);
        store32(buffer + 12, width);
        store32(buffer + 16, height);
        for (uint32_t i = 5; i < 64; i++)
            store32(buffer + i * 4, 0);
    }
    // Re-simulated rollback frames are not presented.
    if (!rollback::resimulating())
    {
        g_frames++;
        netglue::on_present();
    }
    ctx.r3.u64 = 0;
}
