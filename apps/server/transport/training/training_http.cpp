module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.training_http;

import meowlive.standard;
import meowlive.adapters.speech.wav;
import meowlive.application.speech_worker;
import meowlive.application.voice_models;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.speech_voices;
import meowlive.server.training_worker;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.training_job_http;

namespace meowlive::server::transport
{
    drogon::HttpResponsePtr voice_model_error(meowlive::application::VoiceModelError error);

    void respond_voice_models(const drogon::HttpRequestPtr& request,
                              const std::shared_ptr<meowlive::server::TrainingWorker>& worker,
                              meowlive::application::WorkQueue& work,
                              const meowlive::server::transport::HttpSend& send);
}

namespace
{
    constexpr std::string_view transcribe_path = "/api/training/transcribe";
    constexpr std::string_view components_path = "/api/training/components";
    constexpr std::size_t audio_limit = 2 * 1024 * 1024;
    constexpr std::size_t metadata_limit = 1024;

    drogon::HttpResponsePtr invalid(std::string_view detail)
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_request", std::string(detail));
    }

    drogon::HttpResponsePtr
    transcription_response(const meowlive::server::TrainingWorker::Result& result)
    {
        if (!result)
        {
            using Failure = meowlive::server::TranscriptionFailureKind;
            switch (result.error().kind)
            {
            case Failure::OperationFailed:
                return meowlive::server::transport::error_response(
                    drogon::k502BadGateway, "transcription_failed", result.error().message);
            case Failure::OutOfMemory:
                return meowlive::server::transport::internal_error_response(
                    meowlive::platform::ExceptionKind::OutOfMemory);
            case Failure::InternalError:
                return meowlive::server::transport::internal_error_response(
                    meowlive::platform::ExceptionKind::Unknown);
            }
            std::unreachable();
        }
        ::Json::Value transcript(::Json::objectValue);
        transcript["text"] = result->text;
        transcript["language"] = result->language;
        if (result->model)
        {
            transcript["asr_model_id"] = result->model->id;
            transcript["asr_model_revision"] = result->model->revision;
        }
        return drogon::HttpResponse::newHttpJsonResponse(std::move(transcript));
    }
}

namespace meowlive::server::transport
{
    bool is_training_upload(std::string_view path, drogon::HttpMethod method)
    {
        return (path == transcribe_path && method == drogon::Post) ||
               meowlive::server::transport::is_training_job_upload(path, method);
    }

    std::array<std::string_view, 11> training_paths()
    {
        const auto jobs = meowlive::server::transport::training_job_paths();
        return {transcribe_path,
                components_path,
                jobs[0],
                jobs[1],
                jobs[2],
                jobs[3],
                jobs[4],
                jobs[5],
                meowlive::server::transport::voice_catalog_path,
                meowlive::server::transport::voice_downloads_path,
                meowlive::server::transport::voice_cancel_path};
    }

    std::string_view training_route_methods(std::string_view path)
    {
        if (path == components_path || path == meowlive::server::transport::voice_catalog_path)
        {
            return "GET,HEAD";
        }
        if (path == meowlive::server::transport::voice_downloads_path)
        {
            return "GET,HEAD,POST";
        }
        return path == transcribe_path || path == meowlive::server::transport::voice_cancel_path
                   ? "POST"
                   : meowlive::server::transport::training_job_methods(path);
    }

    void respond_training(const drogon::HttpRequestPtr& request,
                          const std::shared_ptr<meowlive::server::TrainingWorker>& worker,
                          const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                          const std::shared_ptr<meowlive::server::VoiceLibrary>& voices,
                          meowlive::application::WorkQueue& work,
                          meowlive::application::WorkQueue& auditions, std::stop_token stop,
                          meowlive::server::transport::HttpSend send)
    {
        const auto& path = request->getOriginalPath();
        if (path == meowlive::server::transport::voice_catalog_path ||
            path == meowlive::server::transport::voice_downloads_path ||
            path == meowlive::server::transport::voice_cancel_path)
        {
            meowlive::server::transport::respond_voice_models(request, worker, work, send);
            return;
        }
        if (!meowlive::server::transport::training_job_methods(request->getOriginalPath()).empty())
        {
            const bool audition =
                meowlive::server::transport::is_training_audition(request->getOriginalPath());
            meowlive::server::transport::submit_http_work(
                audition ? auditions : work, request->getMatchedPathPattern(), send,
                [request, worker, speech, voices, audition, stop]
                {
                    if (audition && stop.stop_requested())
                    {
                        return meowlive::server::transport::server_stopping_response();
                    }
                    auto response = meowlive::server::transport::respond_training_job(
                        request, worker->jobs(), speech, *voices,
                        static_cast<bool>(worker->model_control()), stop);
                    return audition && stop.stop_requested()
                               ? meowlive::server::transport::server_stopping_response()
                               : response;
                },
                "training_busy", "本机任务队列繁忙", meowlive::platform::ExceptionDetails::Omit);
            return;
        }
        if (request->getOriginalPath() == components_path)
        {
            Json::Value components(Json::objectValue);
            components["transcription_configured"] = worker->configured();
            send(drogon::HttpResponse::newHttpJsonResponse(std::move(components)));
            return;
        }
        if (request->body().size() > audio_limit + 16 * 1024)
        {
            send(meowlive::server::transport::error_response(
                drogon::k413RequestEntityTooLarge, "invalid_request", "参考录音超过上限"));
            return;
        }

        drogon::MultiPartParser multipart;
        if (multipart.parse(request) != 0 || multipart.getFiles().size() != 1 ||
            multipart.getFiles().front().getItemName() != "audio" ||
            multipart.getParameters().size() != 1)
        {
            send(invalid("请选择一份 WAV 录音和转写语言"));
            return;
        }
        const auto field = multipart.getParameters().find("metadata");
        const auto metadata =
            field != multipart.getParameters().end() && field->second.size() <= metadata_limit
                ? meowlive::protocol::json::parse_object(field->second)
                : std::nullopt;
        if (!metadata || !(*metadata)["language"].isString() ||
            (metadata->size() != 1 &&
             (metadata->size() != 3 || !(*metadata)["asr_model_id"].isString() ||
              !(*metadata)["asr_model_revision"].isString())))
        {
            send(invalid("请选择转写语言，显式选择模型时须同时提交模型 ID 和修订"));
            return;
        }
        const auto language = (*metadata)["language"].asString();
        const auto audio = multipart.getFiles().front().fileContent();
        if (audio.empty() || audio.size() > audio_limit || !meowlive::adapters::decode_wav(audio))
        {
            send(invalid("转写语言或 WAV 录音无效"));
            return;
        }
        std::optional<meowlive::application::InstalledVoiceModel> model;
        if (metadata->size() == 3)
        {
            auto selected = worker->voice_models().resolve_transcription(
                {(*metadata)["asr_model_id"].asString(),
                 (*metadata)["asr_model_revision"].asString()},
                language);
            if (!selected)
            {
                send(meowlive::server::transport::voice_model_error(selected.error()));
                return;
            }
            model = std::move(*selected);
        }
        else
        {
            if (!worker->configured())
            {
                send(meowlive::server::transport::error_response(
                    drogon::k409Conflict, "training_disabled", "未配置 Windows Whisper"));
                return;
            }
            constexpr std::array<std::string_view, 5> languages{"zh", "en", "ja", "ko", "yue"};
            if (!std::ranges::contains(languages, language))
            {
                send(invalid("转写语言无效"));
                return;
            }
        }
        const auto accepted = worker->transcribe(
            drogon::utils::getUuid(), std::string(audio), language, std::move(model),
            [send](meowlive::server::TrainingWorker::Result result) noexcept
            {
                meowlive::server::transport::complete_http_work(
                    transcribe_path, send,
                    [&result]
                    {
                        return transcription_response(result);
                    },
                    meowlive::platform::ExceptionDetails::Omit);
            });
        if (!accepted)
        {
            send(meowlive::server::transport::error_response(drogon::k409Conflict, "training_busy",
                                                             "已有转写任务正在运行"));
        }
    }
}
