module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#if defined(_WIN32)
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#endif

export module meowlive.server.transport.http;

#if !defined(_WIN32)
import std;
#endif

import meowlive.protocol.control;
import meowlive.protocol.json;
import meowlive.server.transport.access;
import meowlive.server.transport.websocket;

namespace
{
    constexpr std::string_view status_path{"/api/status"};
    constexpr std::string_view session_path{"/api/admin/session"};

    drogon::HttpResponsePtr health()
    {
        ::Json::Value content(::Json::objectValue);
        content["service"] = "meowlive";
        content["protocol_version"] = meowlive::protocol::version;
        content["bridge_connected"] = meowlive::server::transport::bridge_snapshot().connected;
        return drogon::HttpResponse::newHttpJsonResponse(std::move(content));
    }

    drogon::HttpResponsePtr status()
    {
        const auto bridge = meowlive::server::transport::bridge_snapshot();
        ::Json::Value content(::Json::objectValue);
        content["protocol_version"] = meowlive::protocol::version;
        content["bridge_connected"] = bridge.connected;
        content["session_id"] = bridge.session_id;
        content["generation"] = bridge.generation;
        content["speeches"] = ::Json::Value(::Json::arrayValue);
        return drogon::HttpResponse::newHttpJsonResponse(std::move(content));
    }

    drogon::HttpResponsePtr invalid_login()
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_request", "需要 token 字符串");
    }

    drogon::HttpResponsePtr login(const drogon::HttpRequestPtr& request,
                                  meowlive::server::transport::Access& access)
    {
        std::string_view media_type = request->getHeader("content-type");
        media_type = media_type.substr(0, media_type.find(';'));
        const auto last = media_type.find_last_not_of(" \t");
        media_type =
            last == std::string_view::npos ? std::string_view{} : media_type.substr(0, last + 1);
        if (media_type != "application/json" &&
            !(media_type.starts_with("application/") && media_type.ends_with("+json")))
        {
            return invalid_login();
        }

        const auto content = meowlive::protocol::json::parse_object(request->body());
        if (!content || content->size() != 1 || !(*content)["token"].isString())
        {
            return invalid_login();
        }

        const auto session = access.login((*content)["token"].asString());
        using Result = meowlive::server::transport::LoginResult;
        switch (session.result)
        {
        case Result::Disabled:
            return meowlive::server::transport::error_response(drogon::k403Forbidden,
                                                               "auth_disabled", "管理员认证未启用");
        case Result::Locked:
            return meowlive::server::transport::authentication_locked();
        case Result::Invalid:
            return meowlive::server::transport::authentication_required();
        case Result::RateLimited:
            return meowlive::server::transport::error_response(
                drogon::k429TooManyRequests, "login_rate_limited", "登录失败次数过多，请稍后重试");
        case Result::Success:
            ::Json::Value answer(::Json::objectValue);
            answer["token"] = session.token;
            answer["expires_in_seconds"] = session.expires_in_seconds;
            return drogon::HttpResponse::newHttpJsonResponse(std::move(answer));
        }
        return invalid_login();
    }

    drogon::HttpResponsePtr admin_session(const drogon::HttpRequestPtr& request,
                                          meowlive::server::transport::Access& access)
    {
        drogon::HttpResponsePtr response;
        switch (request->method())
        {
        case drogon::Get:
        {
            ::Json::Value answer(::Json::objectValue);
            answer["enabled"] = access.enabled();
            answer["authenticated"] =
                !access.enabled() ||
                access.is_admin_session(meowlive::server::transport::bearer(request));
            response = drogon::HttpResponse::newHttpJsonResponse(std::move(answer));
            break;
        }
        case drogon::Post:
            response = login(request, access);
            break;
        case drogon::Delete:
            if (access.enabled() && !access.configured())
            {
                response = meowlive::server::transport::authentication_locked();
            }
            else if (access.revoke(meowlive::server::transport::bearer(request)))
            {
                response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(drogon::k204NoContent);
            }
            else
            {
                response = meowlive::server::transport::authentication_required();
            }
            break;
        default:
            response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k405MethodNotAllowed);
            break;
        }
        response->addHeader("cache-control", "no-store");
        return response;
    }

    drogon::HttpResponsePtr respond(const drogon::HttpRequestPtr& request,
                                    meowlive::server::transport::Access& access)
    {
        if (request->method() == drogon::Options && request->headers().contains("origin") &&
            request->headers().contains("access-control-request-method"))
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            if (access.permits_origin(request->getHeader("origin")))
            {
                response->addHeader("access-control-allow-methods", "GET,POST,DELETE");
                response->addHeader("access-control-allow-headers", "content-type,authorization");
            }
            return response;
        }
        if (auto rejected = meowlive::server::transport::origin_rejection(request, access))
        {
            return rejected;
        }
        if (request->method() == drogon::Options)
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k405MethodNotAllowed);
            return response;
        }
        if (request->path() == meowlive::protocol::health_path)
        {
            return health();
        }
        if (request->path() == session_path)
        {
            return admin_session(request, access);
        }
        if (access.enabled())
        {
            if (!access.configured())
            {
                return meowlive::server::transport::authentication_locked();
            }
            if (!access.is_admin_session(meowlive::server::transport::bearer(request)))
            {
                return meowlive::server::transport::authentication_required();
            }
        }
        return status();
    }
}

export namespace meowlive::server::transport
{
    void register_http(const std::shared_ptr<meowlive::server::transport::Access>& access)
    {
        const auto handler = [access](const drogon::HttpRequestPtr& request,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& send)
        {
            send(respond(request, *access));
        };

        drogon::app()
            .registerHandler(std::string(meowlive::protocol::health_path), auto(handler),
                             {drogon::Get, drogon::Options})
            .registerHandler(std::string(status_path), auto(handler),
                             {drogon::Get, drogon::Options})
            .registerHandler(std::string(session_path), auto(handler),
                             {drogon::Get, drogon::Post, drogon::Delete, drogon::Options})
            .registerPreSendingAdvice(
                [access](const drogon::HttpRequestPtr& request,
                         const drogon::HttpResponsePtr& response)
                {
                    const auto& origin = request->getHeader("origin");
                    if (request->path().starts_with("/api/") || request->path().starts_with("/ws/"))
                    {
                        response->addHeader("vary", "origin, access-control-request-method, "
                                                    "access-control-request-headers");
                        if (access->permits_origin(origin))
                        {
                            response->addHeader("access-control-allow-origin", origin);
                        }
                    }
                });
    }
}
