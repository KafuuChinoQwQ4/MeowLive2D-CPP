module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.profile_http;

import meowlive.standard;

import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.profile_settings;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;

namespace
{
    using Edit = meowlive::server::ProfileEdit;
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;

    struct Route
    {
        std::string_view path;
        Edit edit;
        bool model;
        bool read{};
    };

    constexpr std::array routes{
        Route{"/api/agent/settings", Edit::Save, false},
        Route{"/api/agent/personas", Edit::Create, false, true},
        Route{"/api/agent/personas/select", Edit::Select, false},
        Route{"/api/agent/personas/rename", Edit::Rename, false},
        Route{"/api/agent/personas/update", Edit::Update, false},
        Route{"/api/agent/personas/delete", Edit::Delete, false},
        Route{"/api/llm/settings", Edit::Save, true, true},
        Route{"/api/llm/profiles", Edit::Create, true},
        Route{"/api/llm/profiles/select", Edit::Select, true},
        Route{"/api/llm/profiles/rename", Edit::Rename, true},
        Route{"/api/llm/profiles/delete", Edit::Delete, true},
    };

    const Route* find_route(std::string_view path)
    {
        const auto route = std::ranges::find(routes, path, &Route::path);
        return route == routes.end() ? nullptr : &*route;
    }

    bool valid_request(const Route& route, const ::Json::Value& input)
    {
        if (route.edit == Edit::Save && !route.model)
        {
            return input.isObject();
        }
        switch (route.edit)
        {
        case Edit::Save:
            return meowlive::server::exact_fields(input,
                                                  {"id", "settings", "api_key", "clear_api_key"});
        case Edit::Create:
            return route.model ? meowlive::server::exact_fields(
                                     input, {"name", "settings", "api_key", "clear_api_key"})
                               : meowlive::server::exact_fields(input, {"name", "persona"});
        case Edit::Select:
        case Edit::Delete:
            return meowlive::server::exact_fields(input, {"id"}) && input["id"].isString();
        case Edit::Rename:
            return meowlive::server::exact_fields(input, {"id", "name"}) && input["id"].isString();
        case Edit::Update:
            return meowlive::server::exact_fields(input, {"id", "name", "persona"}) &&
                   input["id"].isString();
        }
        return false;
    }

    drogon::HttpResponsePtr error(const Route& route, meowlive::server::ProfileError result)
    {
        namespace transport = meowlive::server::transport;
        using Error = meowlive::server::ProfileError;
        if (result == Error::Stale)
        {
            return transport::error_response(drogon::k409Conflict,
                                             route.model ? "llm_profile_changed"
                                                         : "persona_profile_changed",
                                             "当前档案已切换，请重新读取后再保存");
        }
        if (result == Error::Storage)
        {
            return transport::error_response(drogon::k500InternalServerError,
                                             route.model ? "llm_settings_save_failed"
                                             : route.edit == Edit::Save
                                                 ? "agent_settings_save_failed"
                                                 : "persona_profiles_unavailable",
                                             "无法保存本机设置");
        }
        return transport::error_response(drogon::k400BadRequest,
                                         route.model                ? "invalid_llm_settings"
                                         : route.edit == Edit::Save ? "invalid_agent_request"
                                                                    : "invalid_persona_profile",
                                         "请求格式、字段或大小无效");
    }
}

export namespace meowlive::server::transport
{
    auto profile_paths()
    {
        std::array<std::string_view, routes.size()> paths;
        std::ranges::transform(routes, paths.begin(), &Route::path);
        return paths;
    }

    std::string_view profile_route_methods(std::string_view path)
    {
        if (const auto* route = find_route(path))
        {
            return route->read ? "GET,HEAD,POST" : "POST";
        }
        return {};
    }

    void respond_profile(const drogon::HttpRequestPtr& request,
                         meowlive::server::ProfileSettings& store, Send send,
                         std::function<::Json::Value()> agent_snapshot)
    {
        const auto* route = find_route(request->getOriginalPath());
        if (!route)
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k404NotFound);
            send(response);
            return;
        }
        if (request->method() == drogon::Get)
        {
            send(drogon::HttpResponse::newHttpJsonResponse(store.snapshot(route->model)));
            return;
        }
        constexpr std::size_t body_limit = 64 * 1024;
        auto input =
            request->body().size() > body_limit || !meowlive::server::transport::is_json_media_type(
                                                       request->getHeader("content-type"))
                ? std::nullopt
                : meowlive::protocol::json::parse_object(request->body());
        if (!input || !valid_request(*route, *input))
        {
            send(error(*route, meowlive::server::ProfileError::Invalid));
            return;
        }
        const auto submitted = store.submit(meowlive::server::transport::make_http_task(
            route->path, send,
            [&store, route, input = std::move(*input), agent_snapshot = std::move(agent_snapshot)]
            {
                auto result = store.edit(route->model, route->edit, input);
                if (!result)
                {
                    return error(*route, result.error());
                }
                return drogon::HttpResponse::newHttpJsonResponse(
                    !route->model && route->edit == Edit::Save ? agent_snapshot()
                                                               : std::move(*result));
            },
            meowlive::platform::ExceptionDetails::Omit));
        meowlive::server::transport::respond_to_http_submission(submitted, send, "settings_busy",
                                                                "本机设置队列繁忙");
    }
}
