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

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

    struct CommandLine
    {
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
    fs::create_directories(native / "hdd", ec);
    fs::create_directories(native / "cache", ec);
    io::mount("\\device\\harddisk0\\partition1", native / "hdd", false);
    io::mount("\\device\\harddisk0\\cache0", native / "cache", false);
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
    xam::g_save_root = native / "save";
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

    log("starting the game at %08X", g_image.entry);
    auto main_thread = kernel::start_thread(g_image.entry, 0, 0, g_image.stack_size, false, "main");

    // TT2_DUMP_THREADS=<seconds>: log where every guest thread is after that long (debugging).
    if (const char* d = std::getenv("TT2_DUMP_THREADS"); d && std::atoi(d) > 0)
        std::thread([seconds = std::atoi(d)] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            kernel::dump_threads();
        }).detach();

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
            SDL_Delay(4);
        if (main_thread->exited)
        {
            log("the game's main thread exited (code %u)", main_thread->exit_code);
            running = false;
        }
    }

    netglue::shutdown();
    log("shutting down");
    if (g_log_file)
        fflush(g_log_file);
    // Guest threads cannot be joined safely; end the process.
    ExitProcess(0);
}
