module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.managed_lifetime;

import meowlive.standard;

import meowlive.platform.exception_diagnostics;
import meowlive.protocol.managed_server;

#if defined(_WIN32)
namespace
{
    std::optional<std::chrono::steady_clock::time_point> update_deadline(std::string_view line)
    {
        std::uint64_t expires_at{};
        const auto end = line.data() + line.size();
        const auto parsed = std::from_chars(line.data(), end, expires_at);
        const auto now = ::GetTickCount64();
        if (parsed.ec != std::errc{} || parsed.ptr != end || expires_at <= now ||
            expires_at - now > meowlive::protocol::managed_server::admission_window_ms)
        {
            return std::nullopt;
        }
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(expires_at - now);
    }

    void write_reply(const char* line, std::string_view deadline)
    {
        std::fprintf(stdout, "%s %.*s\n", line, static_cast<int>(deadline.size()), deadline.data());
        std::fflush(stdout);
    }

    bool
    admit_update(std::string_view deadline_text,
                 const std::function<bool(std::chrono::steady_clock::time_point)>& stop_if_idle)
    {
        const auto deadline = update_deadline(deadline_text);
        if (!deadline)
        {
            return false;
        }
        try
        {
            return stop_if_idle(*deadline);
        }
        catch (...)
        {
            meowlive::platform::log_current_exception("server.update_admission",
                                                      meowlive::platform::ExceptionDetails::Omit);
            return false;
        }
    }
}
#endif

export namespace meowlive::server
{
#if defined(_WIN32)
    std::jthread
    watch_desktop_pipe(std::function<void()> shutdown,
                       std::function<bool(std::chrono::steady_clock::time_point)> stop_if_idle)
    {
        const auto input = ::GetStdHandle(STD_INPUT_HANDLE);
        return std::jthread(
            [input, shutdown = std::move(shutdown),
             stop_if_idle = std::move(stop_if_idle)](std::stop_token stop)
            {
                std::array<char, 64> command{};
                std::size_t length{};
                bool oversized{};
                while (!stop.stop_requested())
                {
                    DWORD available{};
                    if (!::PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
                    {
                        shutdown();
                        return;
                    }
                    if (!available)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                    std::array<char, 64> bytes;
                    DWORD read{};
                    if (!::ReadFile(input, bytes.data(), std::min<DWORD>(available, bytes.size()),
                                    &read, nullptr) ||
                        !read)
                    {
                        shutdown();
                        return;
                    }
                    for (std::size_t index = 0; index < read; ++index)
                    {
                        const auto byte = bytes[index];
                        if (byte != '\n')
                        {
                            if (length == command.size())
                            {
                                oversized = true;
                            }
                            else
                            {
                                command[length++] = byte;
                            }
                            continue;
                        }
                        const std::string_view line(command.data(), length);
                        constexpr std::string_view prefix =
                            meowlive::protocol::managed_server::stop_command;
                        if (!oversized && line.starts_with(prefix))
                        {
                            const auto deadline = line.substr(prefix.size());
                            if (admit_update(deadline, stop_if_idle))
                            {
                                write_reply(meowlive::protocol::managed_server::stopping_reply,
                                            deadline);
                                shutdown();
                                return;
                            }
                            write_reply(meowlive::protocol::managed_server::busy_reply, deadline);
                        }
                        length = 0;
                        oversized = false;
                    }
                }
            });
    }
#endif
}
