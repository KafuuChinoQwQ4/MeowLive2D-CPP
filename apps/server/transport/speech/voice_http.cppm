module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.voice_http;

import meowlive.standard;

import meowlive.adapters.storage.voice_catalog;
import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.domain.speech;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.speech_voices;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;

namespace
{
    constexpr std::string_view resources_path{"/api/resources"};
    constexpr std::string_view upload_path{"/api/voices"};
    constexpr std::string_view select_path{"/api/voices/select"};
    constexpr std::string_view delete_path{"/api/voices/delete"};
    constexpr std::size_t metadata_limit = 16 * 1024;
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;
    using Operation = std::function<meowlive::server::VoiceLibrary::Result()>;

    drogon::HttpResponsePtr respond(meowlive::server::VoiceLibrary::Result result)
    {
        if (result)
        {
            return drogon::HttpResponse::newHttpJsonResponse(std::move(*result));
        }
        using Error = meowlive::server::VoiceEditError;
        const auto [error, detail] = result.error();
        switch (error)
        {
        case Error::Invalid:
            return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                               "invalid_resource", detail);
        case Error::Busy:
            return meowlive::server::transport::error_response(drogon::k409Conflict,
                                                               "resource_busy", detail);
        case Error::Storage:
            return meowlive::server::transport::error_response(drogon::k500InternalServerError,
                                                               "resource_failed", detail);
        }
        std::unreachable();
    }

    drogon::HttpResponsePtr invalid()
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_resource", "资源请求格式无效");
    }

    void submit(meowlive::application::WorkQueue& work, std::string_view route, Operation operation,
                const Send& send)
    {
        meowlive::server::transport::submit_http_work(
            work, route, send,
            [operation = std::move(operation)]
            {
                return respond(operation());
            },
            "resource_busy", "本机资源队列繁忙", meowlive::platform::ExceptionDetails::Omit);
    }

    std::optional<std::string> selection(const drogon::HttpRequestPtr& request)
    {
        if (request->body().size() > metadata_limit ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return {};
        }
        const auto body = meowlive::protocol::json::parse_object(request->body());
        if (!body || body->size() != 1 || !(*body)["id"].isString())
        {
            return {};
        }
        auto id = (*body)["id"].asString();
        return meowlive::domain::valid_voice_id(id) ? std::optional(std::move(id)) : std::nullopt;
    }
}

export namespace meowlive::server::transport
{
    inline constexpr std::size_t voice_body_limit =
        meowlive::adapters::storage::voice_audio_limit + 32 * 1024;

    bool is_voice_upload(std::string_view path, drogon::HttpMethod method)
    {
        return path == upload_path && method == drogon::Post;
    }

    constexpr auto voice_paths()
    {
        return std::array{resources_path, upload_path, select_path, delete_path};
    }

    std::string_view voice_route_methods(std::string_view path)
    {
        if (path == resources_path)
        {
            return "GET,HEAD";
        }
        return path == upload_path || path == select_path || path == delete_path ? "POST" : "";
    }

    void respond_voice(const drogon::HttpRequestPtr& request,
                       const std::shared_ptr<meowlive::server::VoiceLibrary>& library,
                       const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                       meowlive::application::WorkQueue& work, Send send)
    {
        const auto& path = request->getOriginalPath();
        if (path == resources_path)
        {
            submit(
                work, resources_path,
                [library]
                {
                    return library->snapshot();
                },
                send);
            return;
        }
        if (path == upload_path)
        {
            drogon::MultiPartParser multipart;
            if (multipart.parse(request) != 0 || multipart.getFiles().size() != 1 ||
                multipart.getFiles().front().getItemName() != "audio" ||
                multipart.getParameters().size() != 1)
            {
                send(invalid());
                return;
            }
            const auto field = multipart.getParameters().find("metadata");
            if (field == multipart.getParameters().end() || field->second.size() > metadata_limit)
            {
                send(invalid());
                return;
            }
            auto metadata = meowlive::protocol::json::parse_object(field->second);
            if (!metadata)
            {
                send(invalid());
                return;
            }
            submit(
                work, upload_path,
                [library, speech, file = multipart.getFiles().front(),
                 metadata = std::move(*metadata)]() mutable
                {
                    return library->import_voice(*speech, std::move(metadata),
                                                 drogon::utils::getUuid(), file.fileContent());
                },
                send);
            return;
        }
        auto id = selection(request);
        if (!id)
        {
            send(invalid());
            return;
        }
        submit(
            work, path == select_path ? select_path : delete_path,
            [library, speech, id = std::move(*id), select = path == select_path]
            {
                const auto transaction = drogon::utils::getUuid();
                return select ? library->select_voice(*speech, id, transaction)
                              : library->delete_voice(*speech, id, transaction);
            },
            send);
    }
}
