module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.speech_http;

import meowlive.standard;

import meowlive.application.scheduler;
import meowlive.application.speech_worker;
import meowlive.domain.speech;
import meowlive.protocol.json;
import meowlive.protocol.version;
import meowlive.server.transport.access;
import meowlive.server.transport.media_type;

namespace
{
    std::string_view status_name(meowlive::domain::SpeechStatus status)
    {
        using Status = meowlive::domain::SpeechStatus;
        switch (status)
        {
        case Status::Queued:
            return "queued";
        case Status::Synthesizing:
            return "synthesizing";
        case Status::Ready:
            return "ready";
        case Status::Playing:
            return "playing";
        case Status::Completed:
            return "completed";
        case Status::Cancelled:
            return "cancelled";
        case Status::Failed:
            return "failed";
        case Status::Unknown:
            return "unknown";
        }
        std::unreachable();
    }

    ::Json::Value task_json(const meowlive::application::SpeechTask& task)
    {
        ::Json::Value content(::Json::objectValue);
        content["id"] = task.id;
        content["generation"] = task.generation;
        content["text"] = task.text;
        content["voice_id"] = task.voice_id;
        content["status"] = std::string(status_name(task.status));
        content["error"] = task.error ? ::Json::Value(*task.error) : ::Json::Value{};
        return content;
    }

    drogon::HttpResponsePtr enqueue(meowlive::application::SpeechWorker& worker, std::string text,
                                    std::string voice_id)
    {
        auto task = worker.enqueue(std::move(text), std::move(voice_id));
        if (!task)
        {
            using Error = meowlive::application::EnqueueError;
            switch (task.error())
            {
            case Error::Disconnected:
                return meowlive::server::transport::error_response(
                    drogon::k409Conflict, "bridge_disconnected", "执行端未连接");
            case Error::Full:
                return meowlive::server::transport::error_response(drogon::k429TooManyRequests,
                                                                   "queue_full", "播报队列已满");
            case Error::VoiceUnavailable:
                return meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "unknown_voice", "尚未选择可用音色，请先配置参考录音");
            case Error::VoiceEditInProgress:
                return meowlive::server::transport::error_response(
                    drogon::k409Conflict, "voice_edit_in_progress", "音色资源正在更新");
            }
            std::unreachable();
        }
        auto response = drogon::HttpResponse::newHttpJsonResponse(task_json(*task));
        response->setStatusCode(drogon::k202Accepted);
        return response;
    }

    drogon::HttpResponsePtr invalid_speech()
    {
        return meowlive::server::transport::error_response(
            drogon::k400BadRequest, "invalid_request", "需要 text 和 voice_id 字符串");
    }
}

export namespace meowlive::server::transport
{
    drogon::HttpResponsePtr
    speech_status(const meowlive::application::SpeechStatusSnapshot& snapshot)
    {
        ::Json::Value content(::Json::objectValue);
        content["protocol_version"] = meowlive::protocol::version;
        content["bridge_connected"] = snapshot.connected;
        content["session_id"] = snapshot.session_id;
        content["generation"] = snapshot.generation;
        ::Json::Value tasks(::Json::arrayValue);
        for (const auto& task : snapshot.tasks)
        {
            tasks.append(task_json(task));
        }
        content["speeches"] = std::move(tasks);
        return drogon::HttpResponse::newHttpJsonResponse(std::move(content));
    }

    drogon::HttpResponsePtr enqueue_speech(const drogon::HttpRequestPtr& request,
                                           meowlive::application::SpeechWorker& worker)
    {
        constexpr std::size_t body_limit = 16 * 1024;
        if (request->body().size() > body_limit ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return invalid_speech();
        }
        const auto content = meowlive::protocol::json::parse_container(request->body());
        if (!content || (content->isArray() && content->size() != 2))
        {
            return invalid_speech();
        }
        const auto& text = content->isObject() ? (*content)["text"] : (*content)[0u];
        const auto& voice = content->isObject() ? (*content)["voice_id"] : (*content)[1u];
        if (!text.isString() || !voice.isString())
        {
            return invalid_speech();
        }
        auto normalized = meowlive::domain::normalize_speech_text(text.asString());
        if (!normalized)
        {
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "invalid_text", std::string(normalized.error()));
        }
        const auto voice_id = voice.asString();
        if (!meowlive::domain::valid_voice_id(voice_id))
        {
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "invalid_voice",
                "voice ID must contain 1 to 64 ASCII letters, numbers, underscores or hyphens");
        }
        if (voice_id == "preview")
        {
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "unknown_voice", "尚未选择可用音色，请先配置参考录音");
        }
        return enqueue(worker, std::move(*normalized), voice_id);
    }

    drogon::HttpResponsePtr enqueue_preview(meowlive::application::SpeechWorker& worker)
    {
        return enqueue(worker, "试音", "preview");
    }
}
