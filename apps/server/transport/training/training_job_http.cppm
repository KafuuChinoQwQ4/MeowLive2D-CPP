module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.training_job_http;

import meowlive.standard;
import meowlive.adapters.speech.wav;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.application.training_job;
import meowlive.application.voice_models;
import meowlive.protocol.json;
import meowlive.server.speech_voices;
import meowlive.server.training_jobs;
import meowlive.server.transport.access;
import meowlive.server.transport.media_type;
import meowlive.server.transport.training_limits;

namespace
{
    namespace app = meowlive::application;
    namespace json = meowlive::protocol::json;
    namespace server = meowlive::server;
    constexpr std::string_view status_path = "/api/training";
    constexpr std::string_view jobs_path = "/api/training/jobs";
    constexpr std::string_view cancel_path = "/api/training/cancel";
    constexpr std::string_view delete_path = "/api/training/delete";
    constexpr std::string_view activate_path = "/api/training/activate";
    constexpr std::string_view audition_path = "/api/training/audition";
    constexpr std::size_t clip_limit = 2 * 1024 * 1024;
    constexpr std::size_t metadata_limit = 64 * 1024;

    bool valid_recording(std::string_view audio)
    {
        if (audio.empty() || audio.size() > clip_limit)
        {
            return false;
        }
        const auto pcm = meowlive::adapters::decode_wav(audio);
        if (!pcm || pcm->channels < 1 || pcm->channels > 2 || pcm->sample_rate < 8000 ||
            pcm->sample_rate > 48000)
        {
            return false;
        }
        const auto samples_per_second = pcm->channels * pcm->sample_rate;
        return pcm->samples.size() >= samples_per_second * 3 &&
               pcm->samples.size() <= samples_per_second * 10 &&
               std::ranges::any_of(pcm->samples,
                                   [](auto sample)
                                   {
                                       return sample != 0;
                                   });
    }

    std::optional<app::TrainingRequest> parse_upload(const drogon::HttpRequestPtr& request)
    {
        drogon::MultiPartParser upload;
        if (upload.parse(request) != 0 || upload.getParameters().size() != 1 ||
            upload.getFiles().size() < 2 || upload.getFiles().size() > 32)
        {
            return std::nullopt;
        }
        const auto metadata = upload.getParameters().find("metadata");
        if (metadata == upload.getParameters().end() || metadata->second.size() > metadata_limit)
        {
            return std::nullopt;
        }
        const auto value = json::parse_object(metadata->second);
        if (!value || !(*value)["reviewed"].isBool() || !(*value)["reviewed"].asBool() ||
            !(*value)["voice_id"].isString() || !(*value)["name"].isString() ||
            !json::is_unsigned_32((*value)["gpt_epochs"]) ||
            !json::is_unsigned_32((*value)["sovits_epochs"]) || !(*value)["clips"].isArray() ||
            (*value)["clips"].size() != upload.getFiles().size() ||
            (value->isMember("text_mode") && (*value)["text_mode"] != "reviewed_text"))
        {
            return std::nullopt;
        }
        app::TrainingRequest result{.name = (*value)["name"].asString(),
                                    .voice_id = (*value)["voice_id"].asString(),
                                    .gpt_epochs = (*value)["gpt_epochs"].asUInt(),
                                    .sovits_epochs = (*value)["sovits_epochs"].asUInt()};
        const auto& model_id = (*value)["base_model_id"];
        const auto& model_revision = (*value)["base_model_revision"];
        if (value->isMember("base_model_id") || value->isMember("base_model_revision"))
        {
            if (!model_id.isString() || model_id.asString().empty() || !model_revision.isString() ||
                model_revision.asString().empty())
            {
                return std::nullopt;
            }
            result.base_model.emplace(model_id.asString(), model_revision.asString());
            result.model_version.clear();
        }
        if (value->isMember("model_version"))
        {
            if (!(*value)["model_version"].isString() ||
                (*value)["model_version"].asString().empty())
            {
                return std::nullopt;
            }
            result.model_version = (*value)["model_version"].asString();
        }
        constexpr std::array<std::string_view, 5> versions{"v2", "v2Pro", "v2ProPlus", "v3", "v4"};
        if (result.name.empty() || result.name.size() > 240 ||
            (!result.model_version.empty() &&
             !std::ranges::contains(versions, result.model_version)) ||
            result.gpt_epochs < 1 || result.gpt_epochs > 20 || result.sovits_epochs < 1 ||
            result.sovits_epochs > 20)
        {
            return std::nullopt;
        }
        constexpr std::array<std::string_view, 5> languages{"zh", "en", "ja", "ko", "yue"};
        std::size_t total{};
        for (Json::ArrayIndex index = 0; index < (*value)["clips"].size(); ++index)
        {
            const auto& clip = (*value)["clips"][index];
            const auto& file = upload.getFiles()[index];
            const auto audio = file.fileContent();
            total += audio.size();
            if (!clip.isObject() || clip.size() != 2 || !clip["text"].isString() ||
                !clip["language"].isString() || file.getItemName() != "audio" ||
                total > meowlive::server::transport::training_payload_limit ||
                !valid_recording(audio))
            {
                return std::nullopt;
            }
            auto text = clip["text"].asString();
            auto language = clip["language"].asString();
            const auto invalid_text = std::ranges::any_of(text,
                                                          [](unsigned char character)
                                                          {
                                                              return character < 0x20 ||
                                                                     character == 0x7f ||
                                                                     character == '|';
                                                          });
            if (text.empty() || text.size() > 2000 || invalid_text ||
                std::ranges::find(languages, language) == languages.end())
            {
                return std::nullopt;
            }
            result.clips.push_back({std::move(language), std::move(text), std::string(audio)});
        }
        return result;
    }

    std::optional<std::string> selection(const drogon::HttpRequestPtr& request)
    {
        if (request->body().size() > 1024 ||
            !server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return std::nullopt;
        }
        const auto body = json::parse_object(request->body());
        return body && body->size() == 1 && (*body)["id"].isString()
                   ? std::optional{(*body)["id"].asString()}
                   : std::nullopt;
    }

    drogon::HttpResponsePtr audition(const drogon::HttpRequestPtr& request,
                                     server::TrainingJobs& jobs, app::SpeechWorker& speech,
                                     const server::VoiceLibrary& voices, std::stop_token stop)
    {
        const auto failed = []
        {
            return server::transport::error_response(drogon::k409Conflict, "audition_failed",
                                                     "试听失败，请检查训练产物和语音引擎");
        };
        const auto body = request->body().size() <= 4096 && server::transport::is_json_media_type(
                                                                request->getHeader("content-type"))
                              ? json::parse_object(request->body())
                              : std::nullopt;
        if (!body || body->size() != 2 || !(*body)["version_id"].isString() ||
            !(*body)["text"].isString())
        {
            return server::transport::error_response(drogon::k400BadRequest, "invalid_request",
                                                     "请选择训练版本并填写试听文本");
        }
        const auto id = (*body)["version_id"].asString();
        const auto text = (*body)["text"].asString();
        if (text.empty() || text.size() > 2000)
        {
            return failed();
        }
        std::string wav;
        jobs.with_audition_weights(
            id, stop,
            [&](const auto& voice_id, const auto& weights)
            {
                speech.edit_voices(
                    [&]() -> std::optional<app::SpeechVoices>
                    {
                        const auto audio = voices.audition(voice_id, text, weights, stop);
                        if (audio)
                        {
                            if (auto encoded = meowlive::adapters::encode_wav(*audio))
                            {
                                wav = std::move(*encoded);
                            }
                        }
                        return std::nullopt;
                    });
            });
        if (wav.empty())
        {
            return failed();
        }
        auto reply = drogon::HttpResponse::newHttpResponse();
        reply->setContentTypeString("audio/wav");
        reply->setBody(std::move(wav));
        return reply;
    }
}

export namespace meowlive::server::transport
{
    constexpr auto training_job_paths()
    {
        return std::array{status_path, jobs_path,     cancel_path,
                          delete_path, activate_path, audition_path};
    }

    std::string_view training_job_methods(std::string_view path)
    {
        if (path == status_path)
        {
            return "GET,HEAD";
        }
        return path == jobs_path || path == cancel_path || path == delete_path ||
                       path == activate_path || path == audition_path
                   ? "POST"
                   : "";
    }

    bool is_training_job_upload(std::string_view path, drogon::HttpMethod method)
    {
        return path == jobs_path && method == drogon::Post;
    }

    bool is_training_audition(std::string_view path)
    {
        return path == audition_path;
    }

    drogon::HttpResponsePtr respond_training_job(
        const drogon::HttpRequestPtr& request, meowlive::server::TrainingJobs& jobs,
        const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
        const meowlive::server::VoiceLibrary& voices, bool weights_configured, std::stop_token stop)
    {
        const auto& path = request->getOriginalPath();
        if (path == status_path)
        {
            return drogon::HttpResponse::newHttpJsonResponse(jobs.snapshot());
        }
        if (path == audition_path)
        {
            return audition(request, jobs, *speech, voices, stop);
        }
        if (path == jobs_path)
        {
            const auto input = parse_upload(request);
            if (!input || !voices.voice_available(input->voice_id))
            {
                return server::transport::error_response(drogon::k400BadRequest, "invalid_request",
                                                         "请核对音色、片段和校对文本");
            }
            if (!input->base_model && !jobs.configured())
            {
                return server::transport::error_response(drogon::k409Conflict, "training_disabled",
                                                         "未配置 Windows 训练环境");
            }
            const auto result = jobs.start(drogon::utils::getUuid(), *input, speech);
            if (!result)
            {
                if (const auto* message = std::get_if<std::string>(&result.error()))
                {
                    return server::transport::error_response(drogon::k409Conflict,
                                                             "training_failed", *message);
                }
                const auto error = std::get<app::VoiceModelError>(result.error());
                using Error = app::VoiceModelError;
                if (error == Error::UnknownModel)
                {
                    return server::transport::error_response(
                        drogon::k404NotFound, "voice_model_unknown", "找不到所选训练底模");
                }
                if (error == Error::WrongPurpose || error == Error::TrainingVersionMismatch)
                {
                    return server::transport::error_response(
                        drogon::k400BadRequest, "training_base_model_invalid",
                        "所选模型用途或 GPT-SoVITS 版本不匹配");
                }
                return server::transport::error_response(
                    drogon::k409Conflict, "training_base_model_unavailable",
                    "所选底模修订或运行环境不可用，请到可选模型检查");
            }
            auto response = drogon::HttpResponse::newHttpJsonResponse(*result);
            response->setStatusCode(drogon::k202Accepted);
            return response;
        }
        const auto id = selection(request);
        if (!id)
        {
            return server::transport::error_response(drogon::k400BadRequest, "invalid_request",
                                                     "请提供训练任务标识");
        }
        if ((path == cancel_path && jobs.cancel(*id)) || (path == delete_path && jobs.remove(*id)))
        {
            return drogon::HttpResponse::newHttpJsonResponse(jobs.snapshot());
        }
        if (path == activate_path)
        {
            if (!weights_configured)
            {
                return server::transport::error_response(drogon::k409Conflict,
                                                         "model_runtime_unavailable",
                                                         "成对权重切换需要配置独立的本机语音引擎");
            }
            bool changed{};
            const auto idle = speech->edit_voices(
                [&]() -> std::optional<app::SpeechVoices>
                {
                    changed = jobs.activate(*id);
                    return std::nullopt;
                });
            if (idle && changed)
            {
                return drogon::HttpResponse::newHttpJsonResponse(jobs.snapshot());
            }
        }
        return server::transport::error_response(drogon::k409Conflict, "training_failed",
                                                 "训练任务状态不允许此操作");
    }
}
