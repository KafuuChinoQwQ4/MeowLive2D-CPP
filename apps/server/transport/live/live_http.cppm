module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.live_http;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.live;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;

namespace
{
    constexpr std::string_view status_path{"/api/live"};
    constexpr std::string_view settings_path{"/api/live/settings"};
    constexpr std::string_view connect_path{"/api/live/connect"};
    constexpr std::string_view disconnect_path{"/api/live/disconnect"};
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;

    drogon::HttpResponsePtr json(::Json::Value value)
    {
        auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(value));
        response->addHeader("cache-control", "no-store");
        return response;
    }

    drogon::HttpResponsePtr respond(meowlive::server::LiveService::Result result)
    {
        if (result)
        {
            return json(std::move(*result));
        }
        using Error = meowlive::server::LiveError;
        const auto& [kind, detail] = result.error();
        auto status = drogon::k409Conflict;
        std::string_view code;
        switch (kind)
        {
        case Error::Invalid:
            status = drogon::k400BadRequest;
            code = "invalid_live_settings";
            break;
        case Error::Busy:
            code = "live_settings_busy";
            break;
        case Error::NotConfigured:
            code = "live_unconfigured";
            break;
        case Error::Disconnecting:
            code = "live_disconnecting";
            break;
        case Error::Stopping:
            code = "server_stopping";
            break;
        case Error::SaveFailed:
            status = drogon::k500InternalServerError;
            code = "live_settings_save_failed";
            break;
        }
        return meowlive::server::transport::error_response(status, code, detail);
    }
}

export namespace meowlive::server::transport
{
    constexpr auto live_paths()
    {
        return std::array{status_path, settings_path, connect_path, disconnect_path};
    }

    std::string_view live_route_methods(std::string_view path)
    {
        if (path == status_path)
        {
            return "GET,HEAD";
        }
        if (path == settings_path)
        {
            return "GET,HEAD,POST";
        }
        return path == connect_path || path == disconnect_path ? "POST" : "";
    }

    void respond_live(const drogon::HttpRequestPtr& request,
                      const std::shared_ptr<meowlive::server::LiveService>& live,
                      meowlive::application::WorkQueue& work, Send send)
    {
        const auto& path = request->getOriginalPath();
        if (path == status_path)
        {
            send(json(live->status()));
        }
        else if (path == connect_path)
        {
            send(respond(live->connect()));
        }
        else if (path == disconnect_path)
        {
            send(json(live->disconnect()));
        }
        else if (request->method() == drogon::Get)
        {
            send(json(live->settings()));
        }
        else
        {
            constexpr std::size_t body_limit = 16 * 1024;
            auto input = request->body().size() > body_limit ||
                                 !meowlive::server::transport::is_json_media_type(
                                     request->getHeader("content-type"))
                             ? std::nullopt
                             : meowlive::protocol::json::parse_object(request->body());
            if (!input)
            {
                send(respond(std::unexpected(meowlive::server::LiveRejection{
                    meowlive::server::LiveError::Invalid, "直播配置字段无效"})));
                return;
            }
            meowlive::server::transport::submit_http_work(
                work, settings_path, send,
                [live, input = std::move(*input)]
                {
                    return respond(live->save(input));
                },
                "settings_busy", "本机设置队列繁忙", meowlive::platform::ExceptionDetails::Omit);
        }
    }
}
