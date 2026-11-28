module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.training_http;

import meowlive.standard;
import meowlive.application.voice_models;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.server.training_jobs;
import meowlive.server.training_worker;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;
import meowlive.protocol.json;

namespace
{
    Json::Value download_value(const meowlive::application::VoiceDownload& download)
    {
        const auto& progress = download.progress;
        Json::Value result(Json::objectValue);
        result["id"] = download.id;
        result["model_id"] = download.model.id;
        result["revision"] = download.model.revision;
        result["state"] =
            std::string(meowlive::application::voice_download_state_name(progress.state));
        result["received_bytes"] = static_cast<Json::UInt64>(progress.received);
        result["total_bytes"] = static_cast<Json::UInt64>(progress.total);
        result["file"] = progress.file;
        result["message"] = progress.message;
        return result;
    }

    Json::Value catalog_value(const meowlive::application::VoiceModelLibrary& library,
                              bool legacy_whisper, bool legacy_training)
    {
        Json::Value result(Json::objectValue);
        result["revision"] = library.catalog().revision;
        result["legacy_transcription_configured"] = legacy_whisper;
        result["legacy_training_configured"] = legacy_training;
        Json::Value models(Json::arrayValue);
        for (const auto& model : library.catalog().models)
        {
            const auto installation = library.inspect(model);
            Json::Value item(Json::objectValue);
            item["id"] = model.selection.id;
            item["revision"] = model.selection.revision;
            item["name"] = model.name;
            item["purpose"] = model.purpose;
            item["engine_id"] = model.engine_id;
            item["adapter_revision"] = model.adapter_revision;
            item["runtime_id"] = model.runtime_id;
            item["runtime_revision"] = model.runtime_revision;
            item["requirements"] = model.requirements;
            item["license"] = model.license;
            item["source"] = model.source;
            item["installed"] = installation.installed;
            item["ready"] = installation.ready;
            item["downloadable"] = installation.downloadable;
            item["reason"] = installation.reason;
            item["download_bytes"] = static_cast<Json::UInt64>(installation.download_bytes);
            item["installed_bytes"] = static_cast<Json::UInt64>(installation.installed_bytes);
            item["required_free_bytes"] =
                static_cast<Json::UInt64>(installation.required_free_bytes);
            if (model.purpose == "training")
            {
                item["model_version"] = model.model_version;
                item["operations"].append("train");
            }
            else
            {
                item["operations"].append("transcribe");
            }
            Json::Value languages(Json::arrayValue);
            for (const auto& language : model.languages)
            {
                languages.append(language);
            }
            item["languages"] = std::move(languages);
            Json::Value missing(Json::arrayValue);
            for (const auto& file : installation.missing)
            {
                Json::Value asset(Json::objectValue);
                asset["name"] = file.name;
                asset["bytes"] = static_cast<Json::UInt64>(file.bytes);
                missing.append(std::move(asset));
            }
            item["missing"] = std::move(missing);
            models.append(std::move(item));
        }
        result["models"] = std::move(models);
        Json::Value downloads(Json::arrayValue);
        for (const auto& download : library.downloads())
        {
            downloads.append(download_value(download));
        }
        result["downloads"] = std::move(downloads);
        return result;
    }
}

namespace meowlive::server::transport
{
    drogon::HttpResponsePtr voice_model_error(meowlive::application::VoiceModelError error)
    {
        using Error = meowlive::application::VoiceModelError;
        switch (error)
        {
        case Error::UnknownModel:
            return meowlive::server::transport::error_response(
                drogon::k404NotFound, "voice_model_unknown", "找不到所选语音模型");
        case Error::RevisionUnavailable:
            return meowlive::server::transport::error_response(drogon::k409Conflict,
                                                               "voice_model_revision_unavailable",
                                                               "所选模型修订不可用，请刷新列表");
        case Error::WrongPurpose:
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "voice_model_wrong_purpose", "所选模型不适用于该操作");
        case Error::TrainingVersionMismatch:
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "voice_model_version", "请求版本与所选训练底模不匹配");
        case Error::UnsupportedLanguage:
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "voice_model_language", "所选转写模型不支持该语言");
        case Error::NotReady:
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "voice_model_not_ready",
                "语音模型组件缺少或损坏，请打开可选模型检查");
        case Error::InstallUnavailable:
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "voice_install_unavailable",
                "此模型当前无法安装，请查看模型状态");
        case Error::AlreadyInstalled:
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "voice_model_installed", "所选修订已经安装");
        case Error::UnknownDownload:
            return meowlive::server::transport::error_response(
                drogon::k404NotFound, "voice_download_unknown", "未找到下载任务");
        case Error::DownloadFinished:
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "voice_download_finished", "下载任务已结束");
        case Error::Busy:
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "voice_download_busy", "模型下载队列已满，请稍后重试");
        case Error::Closing:
            return meowlive::server::transport::server_stopping_response();
        case Error::Storage:
            return meowlive::server::transport::error_response(
                drogon::k500InternalServerError, "voice_download_storage", "无法持久化下载任务");
        }
        std::unreachable();
    }

    void respond_voice_models(const drogon::HttpRequestPtr& request,
                              const std::shared_ptr<meowlive::server::TrainingWorker>& worker,
                              meowlive::application::WorkQueue& work,
                              const meowlive::server::transport::HttpSend& send)
    {
        meowlive::server::transport::submit_http_work(
            work, request->getOriginalPath(), send,
            [request, worker]() -> drogon::HttpResponsePtr
            {
                auto& library = worker->voice_models();
                const auto& path = request->getOriginalPath();
                if (path == meowlive::server::transport::voice_catalog_path)
                {
                    return drogon::HttpResponse::newHttpJsonResponse(
                        catalog_value(library, worker->configured(), worker->jobs().configured()));
                }
                if (path == meowlive::server::transport::voice_downloads_path &&
                    request->method() != drogon::Post)
                {
                    Json::Value values(Json::arrayValue);
                    for (const auto& download : library.downloads())
                    {
                        values.append(download_value(download));
                    }
                    Json::Value result(Json::objectValue);
                    result["downloads"] = std::move(values);
                    return drogon::HttpResponse::newHttpJsonResponse(std::move(result));
                }
                const auto body = request->body().size() <= 1024 &&
                                          meowlive::server::transport::is_json_media_type(
                                              request->getHeader("content-type"))
                                      ? meowlive::protocol::json::parse_object(request->body())
                                      : std::nullopt;
                if (body && path == meowlive::server::transport::voice_cancel_path &&
                    body->size() == 1 && (*body)["id"].isString())
                {
                    const auto cancelled = library.cancel((*body)["id"].asString());
                    if (!cancelled)
                    {
                        return meowlive::server::transport::voice_model_error(cancelled.error());
                    }
                    Json::Value result(Json::objectValue);
                    result["id"] = (*body)["id"];
                    return drogon::HttpResponse::newHttpJsonResponse(std::move(result));
                }
                if (body && path == meowlive::server::transport::voice_downloads_path &&
                    body->size() == 2 && (*body)["model_id"].isString() &&
                    (*body)["revision"].isString())
                {
                    auto started = library.start_download(
                        {(*body)["model_id"].asString(), (*body)["revision"].asString()},
                        drogon::utils::getUuid());
                    if (!started)
                    {
                        return meowlive::server::transport::voice_model_error(started.error());
                    }
                    Json::Value result(Json::objectValue);
                    result["id"] = std::move(*started);
                    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(result));
                    response->setStatusCode(drogon::k202Accepted);
                    return response;
                }
                return meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "invalid_request", "请选择模型和修订或指定下载任务");
            },
            "voice_model_busy", "本机任务队列繁忙", meowlive::platform::ExceptionDetails::Omit);
    }
}
