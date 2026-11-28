module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.training_http;

import meowlive.standard;
import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.server.speech_voices;
import meowlive.server.training_worker;
import meowlive.server.transport.http_work;

namespace meowlive::server::transport
{
    inline constexpr std::string_view voice_catalog_path = "/api/voice-models";
    inline constexpr std::string_view voice_downloads_path = "/api/voice-models/downloads";
    inline constexpr std::string_view voice_cancel_path = "/api/voice-models/downloads/cancel";

}

export namespace meowlive::server::transport
{
    bool is_training_upload(std::string_view path, drogon::HttpMethod method);
    std::array<std::string_view, 11> training_paths();
    std::string_view training_route_methods(std::string_view path);
    void respond_training(const drogon::HttpRequestPtr& request,
                          const std::shared_ptr<meowlive::server::TrainingWorker>& worker,
                          const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                          const std::shared_ptr<meowlive::server::VoiceLibrary>& voices,
                          meowlive::application::WorkQueue& work,
                          meowlive::application::WorkQueue& auditions, std::stop_token stop,
                          meowlive::server::transport::HttpSend send);
}
