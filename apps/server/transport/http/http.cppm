module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.http;

import meowlive.standard;

import meowlive.adapters.storage.memory_admin;
import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.server.agent_worker;
import meowlive.server.graph_worker;
import meowlive.server.live;
import meowlive.server.profile_settings;
import meowlive.server.speech_voices;
import meowlive.server.training_worker;
import meowlive.server.viewer_admin;
import meowlive.server.transport.access;

namespace meowlive::server::transport
{
    inline constexpr std::string_view speech_status_path = "/api/status";
    inline constexpr std::string_view session_path = "/api/admin/session";
    inline constexpr std::string_view speech_path = "/api/speech";
    inline constexpr std::string_view preview_path = "/api/speech/preview";
    inline constexpr std::string_view stop_path = "/api/stop";
    std::string_view route_methods(std::string_view path);
    bool is_large_http_upload(std::string_view path, bool post);
}

export namespace meowlive::server::transport
{
    struct HttpWorkQueues
    {
        explicit HttpWorkQueues(std::size_t capacity)
            : control(capacity), model_probes(capacity), auditions(capacity)
        {
        }

        void close()
        {
            stopping_.request_stop();
            control.close();
            model_probes.close();
            auditions.close();
        }

        void drain()
        {
            close();
            model_probes.drain();
            auditions.drain();
            control.drain();
        }

        std::stop_token stop_token() const noexcept
        {
            return stopping_.get_token();
        }

        meowlive::application::WorkQueue control;
        meowlive::application::WorkQueue model_probes;
        meowlive::application::WorkQueue auditions;

    private:
        std::stop_source stopping_;
    };

    void register_http(const std::shared_ptr<meowlive::server::transport::Access>& access,
                       const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                       const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                       const std::shared_ptr<meowlive::server::ProfileSettings>& profiles,
                       const std::shared_ptr<meowlive::server::transport::HttpWorkQueues>& queues,
                       const std::shared_ptr<meowlive::server::LiveService>& live,
                       const std::shared_ptr<meowlive::server::VoiceLibrary>& voices,
                       const std::shared_ptr<meowlive::server::TrainingWorker>& training,
                       const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                       const std::shared_ptr<meowlive::adapters::storage::MemoryAdmin>& memory,
                       const std::shared_ptr<meowlive::server::GraphWorker>& graph);
}
