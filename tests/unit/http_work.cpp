#include <cstdio>

#if defined(_WIN32)
#define NOMINMAX
#endif

#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.server.transport.http_response;
import meowlive.server.transport.http_work;
import meowlive.server.transport.storage_http;

namespace
{
    namespace transport = meowlive::server::transport;
    namespace storage = meowlive::adapters::storage;
    namespace diagnostics = meowlive::platform;

    std::array<char, 8'192> log_output{};
    std::size_t log_size{};

    void capture_log(std::string_view line) noexcept
    {
        const auto size = std::min(line.size(), log_output.size() - log_size);
        std::ranges::copy(line.substr(0, size), log_output.begin() + log_size);
        log_size += size;
    }

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    void check_response(const drogon::HttpResponsePtr& response, drogon::HttpStatusCode status,
                        std::string_view code)
    {
        const auto result = response ? response->getJsonObject() : nullptr;
        require(response && response->statusCode() == status && result &&
                    (*result)["code"].asString() == code,
                "unexpected HTTP error response");
    }

    drogon::HttpResponsePtr store_unavailable()
    {
        return transport::error_response(drogon::k503ServiceUnavailable, "store_unavailable",
                                         "store unavailable");
    }

    void check_worker_failures()
    {
        std::array<drogon::HttpResponsePtr, 5> responses;
        std::size_t sent{};
        const transport::HttpSend send = [&](const drogon::HttpResponsePtr& response)
        {
            responses.at(sent++) = response;
        };
        {
            meowlive::application::WorkQueue queue(responses.size());
            transport::submit_storage_http(
                queue, "viewer list", send,
                []() -> drogon::HttpResponsePtr
                {
                    throw storage::PostgresUnavailable("postgres_connection_failed");
                },
                store_unavailable);
            transport::submit_storage_http(
                queue, "viewer list", send,
                []() -> drogon::HttpResponsePtr
                {
                    throw storage::PostgresError("postgres_query_failed_42601");
                },
                store_unavailable);
            transport::submit_http_work(
                queue, "runtime settings", send,
                []() -> drogon::HttpResponsePtr
                {
                    throw std::logic_error("private-settings-marker");
                },
                "settings_busy", "settings busy", diagnostics::ExceptionDetails::Omit);
            transport::submit_http_work(
                queue, "voice snapshot", send,
                []() -> drogon::HttpResponsePtr
                {
                    throw std::bad_alloc{};
                },
                "voice_busy", "voice busy");
            transport::submit_http_work(
                queue, "unexpected error", send,
                []() -> drogon::HttpResponsePtr
                {
                    throw 1;
                },
                "settings_busy", "settings busy");
        }
        require(sent == responses.size(), "each accepted operation sends one reply");
        check_response(responses[0], drogon::k503ServiceUnavailable, "store_unavailable");
        check_response(responses[1], drogon::k500InternalServerError, "internal_error");
        check_response(responses[2], drogon::k500InternalServerError, "internal_error");
        check_response(responses[3], drogon::k500InternalServerError, "out_of_memory");
        check_response(responses[4], drogon::k500InternalServerError, "internal_error");
        const std::string_view logs(log_output.data(), log_size);
        require(logs.contains("operation=viewer list") &&
                    logs.contains("what=postgres_connection_failed") &&
                    logs.contains("what=postgres_query_failed_42601") &&
                    logs.contains("operation=runtime settings exception=std::logic_error") &&
                    logs.contains("operation=voice snapshot exception=std::bad_alloc") &&
                    logs.contains("operation=unexpected error exception=unknown") &&
                    !logs.contains("private-settings-marker") &&
                    !responses[2]->body().contains("private-settings-marker"),
                "exception diagnostics include the operation and type with safe details");
    }

    void check_queue_rejection()
    {
        std::binary_semaphore unblock(0);
        drogon::HttpResponsePtr full;
        drogon::HttpResponsePtr closed;
        {
            meowlive::application::WorkQueue queue(1);
            require(queue.try_submit(
                        [&]() noexcept
                        {
                            unblock.acquire();
                        }) == meowlive::application::WorkSubmission::Accepted,
                    "queue accepts a task");
            const auto operation = []() -> drogon::HttpResponsePtr
            {
                throw std::logic_error("rejected work ran");
            };
            try
            {
                transport::submit_storage_http(
                    queue, "viewer list",
                    [&](const drogon::HttpResponsePtr& response)
                    {
                        full = response;
                    },
                    operation, store_unavailable);
                queue.close();
                transport::submit_storage_http(
                    queue, "viewer list",
                    [&](const drogon::HttpResponsePtr& response)
                    {
                        closed = response;
                    },
                    operation, store_unavailable);
            }
            catch (...)
            {
                unblock.release();
                throw;
            }
            unblock.release();
        }
        check_response(full, drogon::k503ServiceUnavailable, "storage_busy");
        check_response(closed, drogon::k503ServiceUnavailable, "server_stopping");
        require(!std::string_view(log_output.data(), log_size).contains("rejected work ran"),
                "queue rejection leaves the operation unexecuted");
    }

    void check_response_delivery_failure()
    {
        unsigned sent{};
        transport::complete_http_work(
            "response delivery",
            [&](const drogon::HttpResponsePtr&)
            {
                ++sent;
                throw std::runtime_error("delivery failed");
            },
            []
            {
                return drogon::HttpResponse::newHttpResponse();
            });
        require(sent == 1 &&
                    std::string_view(log_output.data(), log_size)
                        .contains("operation=response delivery exception=std::runtime_error"),
                "delivery exceptions are logged without making a second delivery attempt");
    }
}

int main()
{
    diagnostics::set_exception_log_output(capture_log);
    try
    {
        check_worker_failures();
        check_queue_rejection();
        check_response_delivery_failure();
        diagnostics::set_exception_log_output(nullptr);
        return 0;
    }
    catch (const std::exception& error)
    {
        diagnostics::set_exception_log_output(nullptr);
        std::println(stderr, "{}", error.what());
        return 1;
    }
}
