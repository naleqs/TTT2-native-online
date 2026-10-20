// Front end of netplay for the rest of the runtime; the work happens in
// rollback/game_rollback.cpp at the main loop's frame boundary.

#include "netplay_glue.h"
#include "rollback/game_rollback.h"

namespace tt2::netglue
{
    void configure(const Options& options)
    {
        if (options.enabled)
            rollback::configure_netplay(options);
    }

    bool active() { return rollback::netplay_active(); }

    bool input_for_port(int port, input::PadState& out) { return rollback::input_for_port(port, out); }

    int frame(uint64_t* serial) { return rollback::current_frame(serial); }

    void on_present() {}

    std::string status_line() { return rollback::status_line(); }

    void shutdown() { rollback::shutdown(); }
}
