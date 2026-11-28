module;

#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.storage_http;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.server.transport.http_work;

export namespace meowlive::server::transport
{
    template <typename Work>
    void submit_storage_http(meowlive::application::WorkQueue& queue, std::string_view operation,
                             const meowlive::server::transport::HttpSend& send, Work work,
                             drogon::HttpResponsePtr (*unavailable)(),
                             std::source_location source = std::source_location::current())
    {
        meowlive::server::transport::submit_http_work(
            queue, operation, send,
            [operation, work = std::move(work), unavailable, source]() mutable
            {
                try
                {
                    return work();
                }
                catch (const meowlive::adapters::storage::PostgresUnavailable&)
                {
                    meowlive::platform::log_current_exception(
                        operation, meowlive::platform::ExceptionDetails::Include, source);
                    return unavailable();
                }
            },
            "storage_busy", "本机存储任务队列繁忙", meowlive::platform::ExceptionDetails::Include,
            source);
    }
}
