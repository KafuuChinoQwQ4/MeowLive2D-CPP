module;

#include <cstdio>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.live_source;

import meowlive.standard;
import meowlive.adapters.bilibili.packet;
import meowlive.adapters.bilibili.project_api;
import meowlive.adapters.bilibili.transport;
import meowlive.adapters.bilibili.websocket;
import meowlive.application.live;
import meowlive.domain.live_event;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::adapters::bilibili
{
    struct ConnectionOptions
    {
        std::string api_base = "https://live-open.biliapi.com";
        std::chrono::milliseconds request_timeout{10'000};
        std::chrono::milliseconds connect_timeout{10'000};
        std::chrono::milliseconds receive_timeout{45'000};
        std::chrono::milliseconds app_heartbeat_interval{20'000};
        std::chrono::milliseconds websocket_heartbeat_interval{20'000};
    };
}

namespace
{
    namespace app = meowlive::application;
    namespace bili = meowlive::adapters::bilibili;
    namespace domain = meowlive::domain;
    using Clock = std::chrono::steady_clock;
    using Failure = app::LiveSourceFailure;

    struct AuthorizedConnection
    {
        std::unique_ptr<bili::WebSocket> socket;
        bili::Frame initial_frame;
    };

    std::expected<AuthorizedConnection, Failure> authorize(const bili::Project& project,
                                                           const app::LiveCredentials& credentials,
                                                           const bili::ConnectionOptions& options,
                                                           std::stop_token stop)
    {
        const auto authority = bili::encode_packet(7, project.auth_body);
        const auto deadline = Clock::now() + options.connect_timeout;
        bool endpoint_available{};
        for (const auto& url : project.urls)
        {
            if (!bili::safe_endpoint(url, "wss", "ws", options.api_base.starts_with("http://")))
            {
                continue;
            }
            endpoint_available = true;
            auto socket = std::make_unique<bili::WebSocket>();
            if (stop.stop_requested() || !socket->connect(url, deadline, stop) ||
                !socket->send(authority, deadline, stop))
            {
                continue;
            }

            while (!stop.stop_requested() && Clock::now() < deadline)
            {
                auto message = socket->receive();
                if (!message)
                {
                    break;
                }
                if (*message)
                {
                    auto frame = bili::decode_frame(**message, project.room, credentials.app_id,
                                                    project.game_id);
                    if (!frame || frame->authorized == false)
                    {
                        return std::unexpected(Failure::Authorization);
                    }
                    if (frame->authorized == true)
                    {
                        return AuthorizedConnection{std::move(socket), std::move(*frame)};
                    }
                }
                else if (!socket->wait_readable(deadline))
                {
                    break;
                }
            }
        }
        if (stop.stop_requested())
        {
            return std::unexpected(Failure::Stopped);
        }
        return std::unexpected(endpoint_available ? Failure::Disconnected : Failure::Authorization);
    }

    bool deliver(bili::Frame& frame, std::stop_token stop,
                 const std::function<void(domain::LiveEvent)>& event)
    {
        for (auto& notice : frame.events)
        {
            if (stop.stop_requested())
            {
                return false;
            }
            event(std::move(notice));
        }
        return !frame.interaction_ended;
    }

    Failure receive_socket_events(bili::WebSocket& socket, const bili::Project& project,
                                  const app::LiveCredentials& credentials,
                                  const bili::ConnectionOptions& options, std::stop_token stop,
                                  const std::function<void(domain::LiveEvent)>& event,
                                  const std::atomic_bool& heartbeat_failed)
    {
        const auto heartbeat = bili::encode_packet(2);
        auto last_heartbeat = Clock::now();
        auto next_ping = last_heartbeat + options.websocket_heartbeat_interval;
        while (!stop.stop_requested())
        {
            const auto now = Clock::now();
            const auto deadline = last_heartbeat + options.receive_timeout;
            if (heartbeat_failed.load() || now >= deadline)
            {
                return Failure::Disconnected;
            }
            if (now >= next_ping)
            {
                if (!socket.send(heartbeat, now + options.request_timeout, stop))
                {
                    return stop.stop_requested() ? Failure::Stopped : Failure::Disconnected;
                }
                next_ping = Clock::now() + options.websocket_heartbeat_interval;
            }

            auto message = socket.receive();
            if (!message)
            {
                return Failure::Disconnected;
            }
            if (*message)
            {
                auto frame = bili::decode_frame(**message, project.room, credentials.app_id,
                                                project.game_id);
                if (!frame)
                {
                    return Failure::Disconnected;
                }
                if (frame->heartbeat)
                {
                    last_heartbeat = Clock::now();
                }
                if (!deliver(*frame, stop, event))
                {
                    return stop.stop_requested() ? Failure::Stopped : Failure::Authorization;
                }
            }
            else if (!socket.wait_readable(next_ping < deadline ? next_ping : deadline))
            {
                return Failure::Disconnected;
            }
        }
        return Failure::Stopped;
    }

    Failure receive_events(AuthorizedConnection& connection, bili::ProjectApi& api,
                           const bili::Project& project, const app::LiveCredentials& credentials,
                           const bili::ConnectionOptions& options, std::stop_token stop,
                           const std::function<void(domain::LiveEvent)>& event)
    {
        if (!deliver(connection.initial_frame, stop, event))
        {
            return stop.stop_requested() ? Failure::Stopped : Failure::Authorization;
        }

        std::atomic_bool heartbeat_failed{};
        std::exception_ptr heartbeat_exception;
        std::jthread heartbeat_worker(
            [&](std::stop_token end)
            {
                try
                {
                    std::mutex mutex;
                    std::unique_lock lock(mutex);
                    std::condition_variable_any timer;
                    while (!end.stop_requested())
                    {
                        timer.wait_for(lock, end, options.app_heartbeat_interval,
                                       []
                                       {
                                           return false;
                                       });
                        if (!end.stop_requested() && !api.heartbeat(project.game_id, end))
                        {
                            heartbeat_failed.store(true);
                            break;
                        }
                    }
                }
                catch (...)
                {
                    meowlive::platform::log_current_exception(
                        "Bilibili project heartbeat", meowlive::platform::ExceptionDetails::Omit);
                    heartbeat_exception = std::current_exception();
                    heartbeat_failed.store(true);
                }
            });

        const auto result = receive_socket_events(*connection.socket, project, credentials, options,
                                                  stop, event, heartbeat_failed);
        heartbeat_worker.request_stop();
        heartbeat_worker.join();
        if (heartbeat_exception)
        {
            std::rethrow_exception(heartbeat_exception);
        }
        return result;
    }

    Failure run_authorized(AuthorizedConnection& connection, bili::ProjectApi& api,
                           const bili::Project& project, const app::LiveCredentials& credentials,
                           const bili::ConnectionOptions& options, std::stop_token stop,
                           const std::function<void(std::string)>& connected,
                           const std::function<void(domain::LiveEvent)>& event,
                           const std::function<void()>& lost)
    {
        std::exception_ptr failure;
        auto result = Failure::Disconnected;
        try
        {
            connected(std::to_string(project.room));
            result = receive_events(connection, api, project, credentials, options, stop, event);
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        try
        {
            lost();
        }
        catch (...)
        {
            if (failure)
            {
                meowlive::platform::log_current_exception(
                    "Bilibili connection lost callback",
                    meowlive::platform::ExceptionDetails::Omit);
            }
            else
            {
                failure = std::current_exception();
            }
        }
        connection.socket->close();
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        return result;
    }
}

export namespace meowlive::adapters::bilibili
{
    meowlive::application::LiveSourceFailure
    run_connection(const meowlive::application::LiveCredentials& credentials, std::stop_token stop,
                   const std::function<void(std::string)>& connected,
                   const std::function<void(meowlive::domain::LiveEvent)>& event,
                   const std::function<void()>& lost,
                   const meowlive::adapters::bilibili::ConnectionOptions& options)
    {
        if (stop.stop_requested())
        {
            return Failure::Stopped;
        }
        if (!credentials.app_id || credentials.identity_code.empty() ||
            credentials.identity_code.size() > 512 || options.api_base.contains('?') ||
            !bili::safe_endpoint(options.api_base, "https", "http", true))
        {
            return Failure::Authorization;
        }

        bili::ProjectApi api(credentials, options.api_base, options.request_timeout);
        const auto project = api.start();
        if (!project)
        {
            return project.error();
        }
        std::exception_ptr failure;
        auto result = Failure::Disconnected;
        try
        {
            if (auto connection = authorize(*project, credentials, options, stop))
            {
                result = run_authorized(*connection, api, *project, credentials, options, stop,
                                        connected, event, lost);
            }
            else
            {
                result = connection.error();
            }
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        try
        {
            if (!api.end(project->game_id))
            {
                if (!failure)
                {
                    return Failure::Cleanup;
                }
                std::fputs("Bilibili project cleanup failed after a session exception\n", stderr);
            }
        }
        catch (...)
        {
            if (!failure)
            {
                throw;
            }
            meowlive::platform::log_current_exception("Bilibili project cleanup",
                                                      meowlive::platform::ExceptionDetails::Omit);
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
        return stop.stop_requested() ? Failure::Stopped : result;
    }

    meowlive::application::LiveSourceFailure
    run(const meowlive::application::LiveCredentials& credentials, std::stop_token stop,
        const std::function<void(std::string)>& connected,
        const std::function<void(meowlive::domain::LiveEvent)>& event,
        const std::function<void()>& lost)
    {
        return bili::run_connection(credentials, stop, connected, event, lost, {});
    }
}
