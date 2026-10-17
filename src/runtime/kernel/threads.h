#pragma once

#include "objects.h"

#include <atomic>
#include <thread>

namespace tt2::kernel
{
    struct Thread final : Object
    {
        uint32_t id = 0;
        uint32_t pcr = 0;            // KPCR (r13), followed by the TLS slots and the TEB
        uint32_t stack_base = 0;     // lowest address of the guest stack
        uint32_t stack_size = 0;
        uint32_t static_tls = 0;     // copy of the image's __declspec(thread) data
        uint32_t start_address = 0, start_context = 0, xapi_startup = 0;
        bool exited = false;
        uint32_t exit_code = 0;
        int suspend_count = 0;
        std::thread host;
        std::string name;

        ObjectType type() const override { return ObjectType::Thread; }
        bool is_signaled(uint32_t) const override { return exited; }
        ~Thread() override;
    };

    // Starts a guest thread. With `suspended`, it waits for NtResumeThread.
    std::shared_ptr<Thread> start_thread(uint32_t start_address, uint32_t start_context, uint32_t xapi_startup,
                                         uint32_t stack_size, bool suspended, const char* name);

    // Gives the calling host thread a guest identity (PCR, TLS, stack) so it can call guest
    // code: used for runtime threads that run guest callbacks (interrupts, audio).
    std::shared_ptr<Thread> adopt_host_thread(const char* name);

    std::shared_ptr<Thread> current_thread();

    // Thrown by ExTerminateThread and caught at the top of the thread.
    struct ThreadExit
    {
        uint32_t code;
    };
}
