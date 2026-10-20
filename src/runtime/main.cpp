// tt2.exe: the recompiled Tekken Tag Tournament 2 with its native runtime.
//
//   tt2.exe [--fullscreen]
//           [--netplay --player 1|2 --local-port N --peer HOST:PORT [--delay N] [--build-id HEX]]
//
// Normally started by the launcher. Paths are relative to the executable (native\bin):
//   native\game   verified disc data       native\save   save data
//   native\cache  game cache partition     native\logs   tt2.log

#include "guest.h"
#include "image.h"
#include "memory.h"
#include "netplay_glue.h"
#include "rollback/game_rollback.h"
#include "audio/audio.h"
#include "gpu/capture.h"
#include "gpu/renderer.h"
#include "gpu/video.h"
#include "input/input.h"
#include "kernel/io.h"
#include "kernel/threads.h"

#include <atomic>
#include <thread>
#include <set>
#include <map>
#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tt2
{
    void timebase_start();
    void timebase_stop();
    namespace xam
    {
        extern fs::path g_save_root;
        extern fs::path g_dlc_root;
        extern std::string g_names[2];
    }

    bool g_trace_imports = false;
    namespace
    {
        std::vector<std::string> g_trace_filter; // empty = every import
        bool g_trace_stack = false;
        uint32_t g_trace_r3 = 0;                 // TT2_TRACE_R3: only calls whose first argument matches
    }

    // The game's code saves return addresses on the guest stack as on the console
    // (stw r12,-8(r1) before stwu r1,-N(r1)), so the back chain gives a guest backtrace.
    std::string guest_backtrace(uint32_t r1, int depth)
    {
        std::string out;
        for (int i = 0; i < depth; i++)
        {
            if (r1 < 0x40000000 || r1 >= 0x7F000000)
                break;
            const uint32_t caller = load32(r1);
            if (caller <= r1 || caller >= 0x7F000000)
                break;
            const uint32_t lr = load32(caller - 8);
            char buf[16];
            snprintf(buf, sizeof(buf), " %08X", lr);
            out += buf;
            r1 = caller;
        }
        return out;
    }

    // TT2_PROFILE=1: samples the main thread's guest call stack every 2 ms and, every 5 seconds,
    // logs the call sites seen in most samples (finding the code that runs in a given scene).
    void start_profiler(std::shared_ptr<kernel::Thread> thread)
    {
        const char* v = std::getenv("TT2_PROFILE");
        if (!v || *v != '1')
            return;
        std::thread([thread] {
            int window = 0;
            for (;;)
            {
                std::map<uint32_t, int> counts;
                int samples = 0;
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < end)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    // The game's main loop runs on its "nuscMain" thread.
                    PPCContext* ctx = nullptr;
                    kernel::for_each_thread([&](kernel::Thread& t) {
                        if (t.name == "nuscMain")
                            ctx = t.context;
                    });
                    if (!ctx)
                        ctx = thread->context;
                    if (!ctx)
                        continue;
                    samples++;
                    std::set<uint32_t> seen;
                    uint32_t r1 = ctx->r1.u32;
                    for (int i = 0; i < 24; i++)
                    {
                        if (r1 < 0x40000000 || r1 >= 0x7F000000)
                            break;
                        const uint32_t caller = load32(r1);
                        if (caller <= r1 || caller >= 0x7F000000)
                            break;
                        const uint32_t lr = load32(caller - 8);
                        if (lr >= 0x82000000 && lr < 0x84000000 && seen.insert(lr).second)
                            counts[lr]++;
                        r1 = caller;
                    }
                }
                std::vector<std::pair<int, uint32_t>> top;
                for (auto [a, n] : counts)
                    top.push_back({ n, a });
                std::sort(top.rbegin(), top.rend());
                std::string line;
                for (size_t i = 0; i < top.size() && i < 80; i++)
                {
                    char buf[24];
                    snprintf(buf, sizeof(buf), " %08X:%d", top[i].second, top[i].first);
                    line += buf;
                }
                static uint64_t reads_before = 0;
                const uint64_t reads = kernel::g_file_reads.load();
                log("profile %d (%d samples, video frame %llu, %llu file reads):%s", window++, samples, (unsigned long long)video::frames(),
                    (unsigned long long)(reads - reads_before), line.c_str());
                reads_before = reads;
            }
        }).detach();
    }

    void trace_import(const char* name, const PPCContext& ctx)
    {
        if (!g_trace_filter.empty() && std::find(g_trace_filter.begin(), g_trace_filter.end(), name) == g_trace_filter.end())
            return;
        if (g_trace_r3 && ctx.r3.u32 != g_trace_r3)
            return;
        const auto t = kernel::current_thread();
        log("[t%u] %s(%08X, %08X, %08X, %08X) lr=%08llX%s%s", t ? t->id : 0, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            (unsigned long long)ctx.lr, g_trace_stack ? " stack:" : "", g_trace_stack ? guest_backtrace(ctx.r1.u32, 12).c_str() : "");
    }

    namespace
    {
        std::mutex g_log_mutex;
        FILE* g_log_file = nullptr;
    }

    void log(const char* fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard lock(g_log_mutex);
        const auto t = GetTickCount64();
        fprintf(stderr, "[%7.3f] %s\n", double(t % 10000000) / 1000.0, buf);
        if (g_log_file)
        {
            fprintf(g_log_file, "[%7.3f] %s\n", double(t % 10000000) / 1000.0, buf);
            fflush(g_log_file);
        }
    }

    [[noreturn]] void fatal(const char* fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log("FATAL: %s", buf);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TTT2 Native Online", buf, nullptr);
        ExitProcess(1);
    }
}

using namespace tt2;

extern "C" [[noreturn]] void tt2_missing_function(uint32_t target, uint64_t lr)
{
    const auto t = kernel::current_thread();
    fatal("The game called %08X (from %08llX, thread %u), which is not a recompiled function.\n"
          "This usually means a function boundary was missed; add it to config/recomp/tt2_hooks.toml.",
          target, (unsigned long long)lr, t ? t->id : 0);
}

namespace
{
    fs::path exe_dir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    // Faults inside the guest address space are reported with guest addresses.
    LONG CALLBACK crash_handler(EXCEPTION_POINTERS* info)
    {
        const auto code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
            code != EXCEPTION_STACK_OVERFLOW && code != 0x80000003 /* breakpoint (debugtrap) */)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t addr = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
        const bool in_guest = g_base && addr >= uintptr_t(g_base) && addr < uintptr_t(g_base) + PPC_MEMORY_SIZE;
        const auto thread = kernel::current_thread();
        PPCContext* ctx = current_context();
        log("CRASH: exception %08lX at host %p (in sub_%08X), %s address %08llX, guest thread %u, lr %08llX r1 %08X, stack:%s",
            code, info->ExceptionRecord->ExceptionAddress, kernel::guest_function_at(uintptr_t(info->ExceptionRecord->ExceptionAddress)),
            in_guest ? "guest" : "host",
            (unsigned long long)(in_guest ? addr - uintptr_t(g_base) : addr), thread ? thread->id : 0,
            (unsigned long long)(ctx ? ctx->lr : 0), ctx ? ctx->r1.u32 : 0, ctx ? guest_backtrace(ctx->r1.u32, 12).c_str() : "");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // TT2_WATCH=<guest address, hex>: log the functions that write that page (debugging).
    // The page is made read-only; a write is logged, the page opened for that one instruction
    // and protected again by a single-step trap, so every later writer is caught too.
    uint32_t g_watch = 0, g_watch_exact = 0;

    thread_local bool t_watch_step = false;

    LONG CALLBACK watch_handler(EXCEPTION_POINTERS* info)
    {
        // The single step after a logged write: protect the page again at once.
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_step)
        {
            t_watch_step = false;
            DWORD old;
            VirtualProtect(g_base + g_watch, 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2 ||
            info->ExceptionRecord->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t addr = info->ExceptionRecord->ExceptionInformation[1];
        if (addr < uintptr_t(g_base) + g_watch || addr >= uintptr_t(g_base) + g_watch + 0x1000)
            return EXCEPTION_CONTINUE_SEARCH;
        PPCContext* ctx = current_context();
        static std::atomic<int> logged{ 0 };
        // A watch address that is not page aligned: only writes to its 16 bytes are logged.
        const uint32_t guest = uint32_t(addr - uintptr_t(g_base));
        bool wanted = !(g_watch_exact & 0xFFF) || (guest & ~0xFu) == (g_watch_exact & ~0xFu);
        // TT2_WATCH_FOREIGN=1: only writes by a thread other than the one whose stack this is
        // (a late write into a stack frame that is gone, by the GPU thread or a worker).
        static const bool foreign_only = [] { const char* v = std::getenv("TT2_WATCH_FOREIGN"); return v && *v == '1'; }();
        if (foreign_only)
        {
            const auto me = kernel::current_thread();
            bool own = false;
            if (me && guest >= me->stack_base && guest < me->stack_base + me->stack_size)
                own = true;
            wanted = wanted && !own;
            if (wanted && logged < 64)
                log("watch:   foreign writer: thread %u '%s' (stack %08X-%08X)", me ? me->id : 0, me ? me->name.c_str() : "host",
                    me ? me->stack_base : 0, me ? me->stack_base + me->stack_size : 0);
        }
        if (wanted && logged++ < 64)
            log("watch: write to %08llX in sub_%08X, lr %08llX, stack:%s", (unsigned long long)(addr - uintptr_t(g_base)),
                kernel::guest_function_at(uintptr_t(info->ExceptionRecord->ExceptionAddress)), (unsigned long long)(ctx ? ctx->lr : 0),
                ctx ? guest_backtrace(ctx->r1.u32, 8).c_str() : "");
        if (wanted && ctx && logged <= 64)
            log("watch:   r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X r31 %08X", ctx->r3.u32, ctx->r4.u32, ctx->r5.u32,
                ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32, ctx->r10.u32, ctx->r11.u32, ctx->r31.u32);
        DWORD old;
        VirtualProtect(g_base + g_watch, 0x1000, PAGE_READWRITE, &old);
        // Let the write happen, then trap right after it (trap flag) to protect the page again.
        info->ContextRecord->EFlags |= 0x100;
        t_watch_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void start_watch()
    {
        const char* v = std::getenv("TT2_WATCH");
        if (!v || !*v)
            return;
        g_watch_exact = uint32_t(std::strtoul(v, nullptr, 16));
        g_watch = g_watch_exact & ~0xFFFu;
        AddVectoredExceptionHandler(1, watch_handler);
        // TT2_WATCH_DELAY=seconds: start watching later (skips start-up writes such as stack setup).
        const char* d = std::getenv("TT2_WATCH_DELAY");
        const int delay = d ? std::atoi(d) : 0;
        std::thread([delay] {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            DWORD old;
            VirtualProtect(g_base + g_watch, 0x1000, PAGE_READONLY, &old);
            log("watch: page %08X", g_watch);
        }).detach();
    }

    // TT2_TIMER_SCAN=video_frame (debugging): from that frame on, find a heap word that counts
    // down by one per frame (the round timer) and watch who writes it.
    void start_timer_scan()
    {
        const char* v = std::getenv("TT2_TIMER_SCAN");
        if (!v || !*v)
            return;
        const uint64_t at = std::strtoull(v, nullptr, 10);
        std::thread([at] {
            auto wait_frame = [](uint64_t f) {
                while (video::frames() < f)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            };
            wait_frame(at);
            std::vector<std::pair<uint32_t, int32_t>> cands;
            uint64_t f0 = video::frames();
            for (auto [s, n] : g_memory.virt.ranges())
                for (uint32_t a = s; a + 4 <= s + n; a += 4)
                {
                    const int32_t x = int32_t(load32(a));
                    if (x >= 1000 && x <= 5000)
                        cands.push_back({ a, x });
                }
            log("timer scan: %zu candidates at frame %llu", cands.size(), (unsigned long long)f0);
            for (int pass = 0; pass < 3 && cands.size() > 1; pass++)
            {
                wait_frame(f0 + 60);
                const uint64_t f1 = video::frames();
                const int32_t step = int32_t(f1 - f0);
                std::vector<std::pair<uint32_t, int32_t>> next;
                for (auto [a, x] : cands)
                {
                    const int32_t y = int32_t(load32(a));
                    if (x - y >= step - 3 && x - y <= step + 3)
                        next.push_back({ a, y });
                }
                cands.swap(next);
                f0 = f1;
                log("timer scan: %zu left after %d frames", cands.size(), step);
            }
            for (size_t i = 0; i < cands.size() && i < 10; i++)
                log("timer scan: %08X = %d", cands[i].first, cands[i].second);
            if (!cands.empty())
            {
                g_watch_exact = cands[0].first;
                g_watch = g_watch_exact & ~0xFFFu;
                AddVectoredExceptionHandler(1, watch_handler);
                DWORD old;
                VirtualProtect(g_base + g_watch, 0x1000, PAGE_READONLY, &old);
            }
        }).detach();
    }

    // Exit code that tells the launcher to open the online lobby (the game's Back shortcut).
    constexpr DWORD kExitToLobby = 0x4C4F;

    // Holding Back for 2 seconds outside a netplay match opens the online lobby: the game closes
    // and the launcher shows the lobby (netplay games are started from boot by the launcher).
    // True once the button has been held long enough.
    bool lobby_shortcut_held()
    {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point since{};
        static bool holding = false;
        if (netglue::active())
            return false;
        bool back = false;
        for (int port = 0; port < 2; port++)
            back = back || (input::read(port).buttons & 0x0020) != 0;
        if (!back)
        {
            holding = false;
            return false;
        }
        if (!holding)
        {
            holding = true;
            since = Clock::now();
        }
        return Clock::now() - since >= std::chrono::seconds(2);
    }

    struct CommandLine
    {
        bool from_launcher = false;     // started by the launcher (it opens the lobby on kExitToLobby)
        bool fullscreen = false;
        netglue::Options net;
    };

    CommandLine parse(int argc, char** argv)
    {
        CommandLine c;
        for (int i = 1; i < argc; i++)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--fullscreen")
                c.fullscreen = true;
            else if (a == "--from-launcher")
                c.from_launcher = true;
            else if (a == "--netplay")
                c.net.enabled = true;
            else if (a == "--player")
                c.net.local_player = std::stoi(next()) == 2 ? 1 : 0;
            else if (a == "--local-port")
                c.net.local_port = uint16_t(std::stoi(next()));
            else if (a == "--peer")
            {
                const std::string p = next();
                const auto colon = p.rfind(':');
                c.net.remote_host = p.substr(0, colon);
                if (colon != std::string::npos)
                    c.net.remote_port = uint16_t(std::stoi(p.substr(colon + 1)));
            }
            else if (a == "--delay")
                c.net.input_delay = std::stoi(next());
            else if (a == "--build-id")
                c.net.build_id = std::stoull(next(), nullptr, 16);
            else if (a == "--name1")
                xam::g_names[0] = next();
            else if (a == "--name2")
                xam::g_names[1] = next();
        }
        return c;
    }

    void draw_placeholder(SDL_Renderer* r, uint64_t frames, const std::string& net)
    {
        SDL_SetRenderDrawColor(r, 12, 12, 18, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 230, 230, 235, 255);
        SDL_SetRenderScale(r, 2.0f, 2.0f);
        SDL_RenderDebugText(r, 20, 20, "TTT2 Native Online");
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        char line[256];
        SDL_RenderDebugText(r, 40, 90, "The game code is running natively. The renderer is not implemented yet,");
        SDL_RenderDebugText(r, 40, 104, "so nothing is drawn here (see docs/STATUS.md).");
        snprintf(line, sizeof(line), "Presented game frames: %llu", (unsigned long long)frames);
        SDL_RenderDebugText(r, 40, 140, line);
        snprintf(line, sizeof(line), "Controllers: %d of 2", input::connected_count());
        SDL_RenderDebugText(r, 40, 154, line);
        snprintf(line, sizeof(line), "Netplay: %s", net.c_str());
        SDL_RenderDebugText(r, 40, 168, line);
        SDL_RenderPresent(r);
    }
}

int main(int argc, char** argv)
{
    const CommandLine cmd = parse(argc, argv);
    // TT2_TRACE=1 logs every import; TT2_TRACE=Name1,Name2 only those. TT2_TRACE_STACK=1 adds
    // a guest backtrace to each line.
    if (const char* t = std::getenv("TT2_TRACE"); t && *t)
    {
        g_trace_imports = true;
        if (std::string(t) != "1")
        {
            std::string list = t;
            size_t start = 0;
            while (start <= list.size())
            {
                const size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty())
                    g_trace_filter.push_back(item);
                if (comma == std::string::npos)
                    break;
                start = comma + 1;
            }
        }
    }
    if (const char* s = std::getenv("TT2_TRACE_STACK"); s && *s == '1')
        g_trace_stack = true;
    if (const char* s = std::getenv("TT2_TRACE_R3"); s && *s)
        g_trace_r3 = uint32_t(std::strtoul(s, nullptr, 16));
    const fs::path bin = exe_dir();
    const fs::path native = bin.parent_path();

    std::error_code ec;
    fs::create_directories(native / "logs", ec);
    // TT2_LOG_NAME: another log file name (two instances on one PC, e.g. a netplay test).
    const char* log_name = std::getenv("TT2_LOG_NAME");
    g_log_file = _wfopen((native / "logs" / (log_name && *log_name ? log_name : "tt2.log")).c_str(), L"w");
    AddVectoredExceptionHandler(1, crash_handler);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
        fatal("SDL_Init failed: %s", SDL_GetError());

    SDL_Window* window = SDL_CreateWindow("Tekken Tag Tournament 2 - Native", 1280, 720, SDL_WINDOW_RESIZABLE | (cmd.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!window)
        fatal("cannot create the game window: %s", SDL_GetError());
    // Direct3D 11 draws the game; without it, a placeholder screen says what is running.
    SDL_Renderer* renderer = nullptr;
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd || !gpu::renderer::init(hwnd, native / "screenshots"))
    {
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer)
            fatal("cannot create the game window: %s", SDL_GetError());
        SDL_SetRenderVSync(renderer, 1);
    }
    SDL_HideCursor();

    input::init();
    audio::init();

    if (!g_memory.init())
        fatal("cannot reserve the 4 GB guest address space (is this a 64-bit Windows with enough page file?)");
    std::string error;
    if (!load_image(bin, error))
        fatal("%s", error.c_str());
    g_memory.fill_function_table();
    audio::init_xma();

    io::mount("\\device\\cdrom0", native / "game", true);
    io::link("game:", "\\device\\cdrom0");
    io::link("d:", "\\device\\cdrom0");
    // TT2_INSTANCE=name: a second game on the same PC (netplay tests) keeps its own hard disk,
    // cache and saves in native\instances\<name>, so the two don't fight over the same files.
    fs::path user_data = native;
    if (const char* inst = std::getenv("TT2_INSTANCE"); inst && *inst)
        user_data = native / "instances" / inst;
    fs::create_directories(user_data / "hdd", ec);
    fs::create_directories(user_data / "cache", ec);
    io::mount("\\device\\harddisk0\\partition1", user_data / "hdd", false);
    io::mount("\\device\\harddisk0\\cache0", user_data / "cache", false);
    io::link("hdd:", "\\device\\harddisk0\\partition1");
    io::link("cache:", "\\device\\harddisk0\\cache0");
#if TT2_TU3
    // The title update's own files (data100.bin/.ofs), which the patched game reads from
    // update:\. Setup names the applied update's folder in native\update\active.txt.
    if (std::ifstream active(native / "update" / "active.txt"); active)
    {
        std::string folder;
        std::getline(active, folder);
        while (!folder.empty() && (folder.back() == '\r' || folder.back() == ' ' || folder.back() == '\n'))
            folder.pop_back();
        if (!folder.empty() && fs::exists(native / "update" / folder, ec))
        {
            io::mount("\\device\\update", native / "update" / folder, true);
            io::link("update:", "\\device\\update");
        }
        else
            log("title update folder '%s' is missing: run Setup.bat again", folder.c_str());
    }
#endif
    xam::g_save_root = user_data / "save";
    xam::g_dlc_root = native / "dlc";
    fs::create_directories(xam::g_save_root, ec);

    gpu::set_shader_dump_dir(native / "gpu" / "shaders");
    timebase_start();
    netglue::configure(cmd.net);
    // TT2_ROLLBACK_STRESS=interval[,depth[,start_frame]]: offline rollback determinism test.
    if (const char* s = std::getenv("TT2_ROLLBACK_STRESS"); s && *s)
    {
        int v[3] = { 10, 3, 0 };
        sscanf(s, "%d,%d,%d", &v[0], &v[1], &v[2]);
        rollback::configure_stress(v[0], v[1], v[2]);
    }

    start_watch();
    start_timer_scan();
    log("starting the game at %08X", g_image.entry);
    auto main_thread = kernel::start_thread(g_image.entry, 0, 0, g_image.stack_size, false, "main");
    start_profiler(main_thread);

    // TT2_DUMP_THREADS=<seconds>: log where every guest thread is after that long (debugging).
    if (const char* d = std::getenv("TT2_DUMP_THREADS"); d && std::atoi(d) > 0)
        std::thread([seconds = std::atoi(d)] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            kernel::dump_threads();
        }).detach();

    DWORD exit_code = 0;
    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_EVENT_QUIT)
                running = false;
            input::handle_event(e);
        }
        input::poll();
        if (renderer)
            draw_placeholder(renderer, video::frames(), netglue::status_line());
        else
            SDL_Delay(1); // controllers are polled every millisecond (the game reads them at frame start)
        if (main_thread->exited)
        {
            log("the game's main thread exited (code %u)", main_thread->exit_code);
            running = false;
        }
        if (lobby_shortcut_held())
        {
            log("Back held: leaving for the online lobby");
            if (!cmd.from_launcher)
            {
                // Started on its own: start the launcher in the lobby.
                const fs::path launcher = native.parent_path() / "TTT2 Native Online.exe";
                if (fs::exists(launcher))
                    ShellExecuteW(nullptr, L"open", launcher.c_str(), L"--lobby", native.parent_path().c_str(), SW_SHOWNORMAL);
                else
                    log("no launcher at %s", launcher.string().c_str());
            }
            exit_code = kExitToLobby;
            running = false;
        }
    }

    netglue::shutdown();
    log("shutting down");
    if (g_log_file)
        fflush(g_log_file);
    // Guest threads cannot be joined safely; end the process.
    ExitProcess(exit_code);
}
