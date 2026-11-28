module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.http;

import meowlive.standard;
import meowlive.platform.exception_diagnostics;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;

namespace meowlive::server::transport
{
    std::string_view http_operation(const drogon::HttpRequestPtr& request);

    void
    bind_http_routes(const std::shared_ptr<meowlive::server::transport::Access>& access,
                     std::span<const std::string_view> paths,
                     const std::function<void(const drogon::HttpRequestPtr&,
                                              meowlive::server::transport::HttpSend&&)>& handler)
    {
        auto& app = drogon::app();
        // A cold transcription worker can use its full 300 second deadline.
        app.setIdleConnectionTimeout(std::chrono::seconds(310));
        app.setExceptionHandler(
            [](const std::exception&, const drogon::HttpRequestPtr& request,
               meowlive::server::transport::HttpSend&& send)
            {
                meowlive::server::transport::send_http_exception(
                    meowlive::server::transport::http_operation(request), send,
                    meowlive::platform::ExceptionDetails::Omit);
            });
        for (const auto path : paths)
        {
            app.registerHandler(std::string(path), auto(handler));
        }
        app.registerHandlerViaRegex("^/api/agent/traces/[A-Fa-f0-9-]+$", auto(handler))
            .registerHandlerViaRegex("^/api/admin/viewers/[A-Fa-f0-9-]+(?:/(?:adjust|reverse))?$",
                                     auto(handler))
            .registerHandlerViaRegex("^/api/admin/viewers/[A-Fa-f0-9-]+/relationships$",
                                     auto(handler))
            .registerHandlerViaRegex("^/api/admin/relationships/[A-Fa-f0-9-]+$", auto(handler))
            .registerHandlerViaRegex("^/api/admin/memories/(?:status|retry|rebuild-vectors)$",
                                     auto(handler))
            .registerHandlerViaRegex(
                "^/api/admin/viewers/[A-Fa-f0-9-]+/memories(?:/[A-Fa-f0-9-]+)?$", auto(handler))
            .registerPreRoutingAdvice(
                [](const drogon::HttpRequestPtr& request, drogon::AdviceCallback&& send,
                   drogon::AdviceChainCallback&& next)
                {
                    constexpr std::size_t request_body_limit = 1024 * 1024;
                    if (request->body().size() > request_body_limit &&
                        !meowlive::server::transport::is_large_http_upload(
                            request->getOriginalPath(), request->method() == drogon::Post))
                    {
                        auto response = drogon::HttpResponse::newHttpResponse();
                        response->setStatusCode(drogon::k413RequestEntityTooLarge);
                        send(response);
                        return;
                    }
                    const auto methods =
                        meowlive::server::transport::route_methods(request->getOriginalPath());
                    if (request->method() != drogon::Options || methods.empty())
                    {
                        next();
                        return;
                    }
                    // Handle these routes before Drogon's automatic OPTIONS response.
                    auto response = drogon::HttpResponse::newHttpResponse();
                    response->addHeader("allow", std::string(methods));
                    response->addHeader("access-control-allow-methods", "GET,POST,DELETE");
                    response->addHeader("access-control-allow-headers",
                                        "content-type,authorization");
                    send(response);
                })
            .registerPreSendingAdvice(
                [access](const drogon::HttpRequestPtr& request,
                         const drogon::HttpResponsePtr& response)
                {
                    const auto& origin = request->getHeader("origin");
                    if (request->path().starts_with("/api/") || request->path().starts_with("/ws/"))
                    {
                        response->addHeader("vary", "origin, access-control-request-method, "
                                                    "access-control-request-headers");
                        response->removeHeader("access-control-allow-origin");
                        if (access->permits_origin(origin))
                        {
                            response->addHeader("access-control-allow-origin", origin);
                        }
                    }
                });
    }
}
