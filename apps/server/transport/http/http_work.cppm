module;

#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.http_work;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.server.transport.http_response;

export namespace meowlive::server::transport
{
    using HttpSend = std::function<void(const drogon::HttpResponsePtr&)>;

    drogon::HttpResponsePtr server_stopping_response()
    {
        return meowlive::server::transport::error_response(drogon::k503ServiceUnavailable,
                                                           "server_stopping", "主服务正在退出");
    }

    void deliver_http_response(std::string_view operation,
                               const meowlive::server::transport::HttpSend& send,
                               const drogon::HttpResponsePtr& response) noexcept
    {
        try
        {
            send(response);
        }
        catch (...)
        {
            // The transport may have committed the response before throwing.
            meowlive::platform::log_current_exception(operation,
                                                      meowlive::platform::ExceptionDetails::Omit);
        }
    }

    // Called with an active exception at the request boundary.
    void send_http_exception(std::string_view operation,
                             const meowlive::server::transport::HttpSend& send,
                             meowlive::platform::ExceptionDetails details,
                             std::source_location source = std::source_location::current()) noexcept
    {
        const auto kind = meowlive::platform::log_current_exception(operation, details, source);
        try
        {
            meowlive::server::transport::deliver_http_response(
                operation, send, meowlive::server::transport::internal_error_response(kind));
        }
        catch (...)
        {
            meowlive::platform::log_current_exception(operation,
                                                      meowlive::platform::ExceptionDetails::Omit);
        }
    }

    template <typename Work>
    void complete_http_work(std::string_view operation,
                            const meowlive::server::transport::HttpSend& send, Work&& work,
                            meowlive::platform::ExceptionDetails details =
                                meowlive::platform::ExceptionDetails::Include,
                            std::source_location source = std::source_location::current()) noexcept
    {
        drogon::HttpResponsePtr response;
        try
        {
            response = std::forward<Work>(work)();
        }
        catch (...)
        {
            meowlive::server::transport::send_http_exception(operation, send, details, source);
            return;
        }
        meowlive::server::transport::deliver_http_response(operation, send, response);
    }

    template <typename Work>
    meowlive::application::WorkQueue::Task
    make_http_task(std::string_view operation, meowlive::server::transport::HttpSend send,
                   Work work,
                   meowlive::platform::ExceptionDetails details =
                       meowlive::platform::ExceptionDetails::Include,
                   std::source_location source = std::source_location::current())
    {
        return [operation, send = std::move(send), work = std::move(work), details,
                source]() mutable noexcept
        {
            meowlive::server::transport::complete_http_work(operation, send, work, details, source);
        };
    }

    void respond_to_http_submission(meowlive::application::WorkSubmission result,
                                    const meowlive::server::transport::HttpSend& send,
                                    std::string_view busy_code, std::string_view busy_message)
    {
        switch (result)
        {
        case meowlive::application::WorkSubmission::Accepted:
            return;
        case meowlive::application::WorkSubmission::Full:
            send(meowlive::server::transport::error_response(drogon::k503ServiceUnavailable,
                                                             busy_code, busy_message));
            return;
        case meowlive::application::WorkSubmission::Closed:
            send(meowlive::server::transport::server_stopping_response());
            return;
        }
        std::unreachable();
    }

    template <typename Work>
    void submit_http_work(meowlive::application::WorkQueue& queue, std::string_view operation,
                          const meowlive::server::transport::HttpSend& send, Work work,
                          std::string_view busy_code, std::string_view busy_message,
                          meowlive::platform::ExceptionDetails details =
                              meowlive::platform::ExceptionDetails::Include,
                          std::source_location source = std::source_location::current())
    {
        const auto result = queue.try_submit(meowlive::server::transport::make_http_task(
            operation, send, std::move(work), details, source));
        meowlive::server::transport::respond_to_http_submission(result, send, busy_code,
                                                                busy_message);
    }
}
