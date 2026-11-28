module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.managed_server;

import meowlive.standard;

export namespace meowlive::protocol::managed_server
{
    // Commands on the managed child's stdin carry a decimal deadline in
    // Windows GetTickCount64 milliseconds. Each reply echoes that deadline.
    inline constexpr char stop_command[] = "stop-if-idle ";
    inline constexpr char busy_reply[] = "MEOWLIVE_UPDATE_BUSY";
    inline constexpr char stopping_reply[] = "MEOWLIVE_UPDATE_STOPPING";
    inline constexpr std::uint64_t admission_window_ms = 2'000;
    inline constexpr int reply_timeout_ms = 3'000;
    inline constexpr int shutdown_timeout_ms = 20'000;
}
