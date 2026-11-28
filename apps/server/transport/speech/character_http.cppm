module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.character_http;

import meowlive.standard;

import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.domain.character;
import meowlive.protocol.json;
import meowlive.protocol.resources;
import meowlive.server.character_profiles;
import meowlive.server.speech_voices;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;
import meowlive.server.transport.websocket;

namespace
{
    using Edit = meowlive::server::CharacterEdit;
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;
    constexpr std::string_view desktop_path{"/api/desktop/resources"};
    constexpr std::array edits{
        std::pair{std::string_view{"/api/characters/save"}, Edit::Save},
        std::pair{std::string_view{"/api/characters/select"}, Edit::Select},
        std::pair{std::string_view{"/api/characters/delete"}, Edit::Delete},
        std::pair{std::string_view{"/api/characters/preview"}, Edit::Preview},
    };

    auto route(std::string_view path)
    {
        return std::ranges::find(edits, path, &decltype(edits)::value_type::first);
    }

    drogon::HttpResponsePtr invalid(std::string_view detail)
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_resource", detail);
    }

    std::optional<::Json::Value> query(const ::Json::Value& input)
    {
        const auto operation = meowlive::protocol::DesktopResourceOperation::decode(input);
        if (operation)
        {
            if (const auto result = meowlive::server::transport::query_resource(
                    *operation, std::chrono::seconds(15)))
            {
                return result->json();
            }
        }
        return std::nullopt;
    }

    drogon::HttpResponsePtr desktop(const ::Json::Value& input)
    {
        if (input["type"] != "list_models" &&
            (input["type"] != "list_hotkeys" || !input["model_id"].isString() ||
             !meowlive::domain::valid_character_text(input["model_id"].asString(), 128)))
        {
            return invalid("请通过角色选择或预览使用执行端能力");
        }
        const auto result = query(input);
        if (result && ((*result)["type"] == "error" ||
                       (input["type"] == "list_models" && (*result)["type"] == "models") ||
                       (input["type"] == "list_hotkeys" && (*result)["type"] == "hotkeys" &&
                        (*result)["model_id"] == input["model_id"])))
        {
            return drogon::HttpResponse::newHttpJsonResponse(*result);
        }
        return meowlive::server::transport::error_response(
            drogon::k502BadGateway, "desktop_resource_failed", "执行端未确认资源结果");
    }

    drogon::HttpResponsePtr edited(meowlive::server::VoiceLibrary::Result result)
    {
        if (result)
        {
            return drogon::HttpResponse::newHttpJsonResponse(std::move(*result));
        }
        const auto [error, detail] = result.error();
        switch (error)
        {
        case meowlive::server::VoiceEditError::Busy:
            return meowlive::server::transport::error_response(drogon::k409Conflict, "speech_busy",
                                                               detail);
        case meowlive::server::VoiceEditError::Storage:
            return meowlive::server::transport::error_response(drogon::k500InternalServerError,
                                                               "resource_save_failed", detail);
        case meowlive::server::VoiceEditError::Invalid:
            return invalid(detail);
        }
        std::unreachable();
    }
}

export namespace meowlive::server::transport
{
    constexpr auto character_paths()
    {
        return std::array{desktop_path, edits[0].first, edits[1].first, edits[2].first,
                          edits[3].first};
    }

    std::string_view character_route_methods(std::string_view path)
    {
        return path == desktop_path || route(path) != edits.end() ? "POST" : "";
    }

    void respond_character(const drogon::HttpRequestPtr& request,
                           const std::shared_ptr<meowlive::server::VoiceLibrary>& library,
                           const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                           meowlive::application::WorkQueue& work, Send send)
    {
        constexpr std::size_t limit = 32 * 1024;
        const auto body = request->body();
        auto input = body.size() <= limit && meowlive::server::transport::is_json_media_type(
                                                 request->getHeader("content-type"))
                         ? meowlive::protocol::json::parse_object(body)
                         : std::nullopt;
        if (!input)
        {
            send(invalid("资源请求格式无效"));
            return;
        }

        const auto edit = route(request->getOriginalPath());
        meowlive::server::transport::submit_http_work(
            work, edit == edits.end() ? desktop_path : edit->first, send,
            [library, speech, edit, input = std::move(*input)]
            {
                if (edit == edits.end())
                {
                    return desktop(input);
                }
                const auto transaction = drogon::utils::getUuid();
                return edited(meowlive::server::edit_character(edit->second, *library, *speech,
                                                               input, transaction, query));
            },
            "settings_busy", "本机设置队列繁忙", meowlive::platform::ExceptionDetails::Omit);
    }
}
