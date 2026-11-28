module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.agent_worker;

import meowlive.standard;

import meowlive.adapters.llm.language_model;
import meowlive.adapters.llm.web_search;
import meowlive.agent.runtime;
import meowlive.agent.session;
import meowlive.application.live_session;
import meowlive.application.llm;
import meowlive.application.memory_lookup;
import meowlive.application.obs_status;
import meowlive.application.reply_completion;
import meowlive.application.speech_worker;
import meowlive.domain.live_event;
import meowlive.domain.speech;
import meowlive.protocol.json;
import meowlive.server.agent_json;
import meowlive.server.agent_model;
import meowlive.server.agent_observability;
import meowlive.server.agent_runtime_settings;

namespace meowlive::server
{
    namespace agent = meowlive::agent;
    namespace application = meowlive::application;
    namespace json = meowlive::server::agent_json;
    using Clock = std::chrono::steady_clock;

    std::uint64_t monotonic_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    std::uint64_t wall_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string phase_name(agent::AgentPhase phase)
    {
        switch (phase)
        {
        case agent::AgentPhase::Paused:
            return "paused";
        case agent::AgentPhase::Waiting:
            return "waiting";
        case agent::AgentPhase::Deciding:
            return "deciding";
        case agent::AgentPhase::Speaking:
            return "speaking";
        }
        std::unreachable();
    }

    ::Json::Value optional_text(const std::string& text)
    {
        return text.empty() ? ::Json::Value{} : ::Json::Value(text);
    }
}

export namespace meowlive::server
{
    class AgentWorker
    {
    public:
        AgentWorker(std::shared_ptr<meowlive::application::SpeechWorker> speech,
                    const std::filesystem::path& directory,
                    meowlive::application::ObsStatusQuery query_obs,
                    std::function<bool()> live_connected,
                    meowlive::application::ViewerMemoryQuery query_memory = {})
            : speech_(std::move(speech)), query_obs_(std::move(query_obs)),
              live_connected_(std::move(live_connected)), query_memory_(std::move(query_memory)),
              runtime_(directory / "settings.json"), observations_(directory),
              traces_(observations_.loaded_traces()), usage_(observations_.loaded_turns())
        {
        }

        ~AgentWorker()
        {
            close();
        }

        void start(const ::Json::Value& settings, const ::Json::Value& model)
        {
            configure(settings);
            if (auto config = meowlive::server::provider_config(model);
                config && !config->model.empty())
            {
                model_timeout_ = config->timeout;
                model_name_ = config->model;
                provider_ = model.get("provider", "custom").asString();
                config->reasoning_provider = provider_;
                model_ = std::move(*config);
                max_retries_ = model.get("max_retries", 1).asUInt();
            }
            worker_ = std::jthread(
                [this](std::stop_token stop)
                {
                    run(stop);
                });
        }

        void configure(const ::Json::Value& settings)
        {
            auto parsed = json::read_settings(settings);
            std::lock_guard lock(mutex_);
            sync_playback();
            cancel_decision();
            session_.configure(std::move(parsed), monotonic_ms());
            changed_.notify_one();
        }

        meowlive::server::AgentRuntimeSettings& runtime_settings()
        {
            return runtime_;
        }

        void close()
        {
            {
                std::lock_guard lock(mutex_);
                cancel_decision();
                worker_.request_stop();
                sync_playback();
                if (!trace_.isNull())
                {
                    finish_trace("cancelled");
                }
            }
            changed_.notify_one();
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

        void pause()
        {
            std::lock_guard lock(mutex_);
            sync_playback();
            cancel_decision();
            session_.set_paused(true, monotonic_ms());
            changed_.notify_one();
        }

        std::string resume()
        {
            std::lock_guard lock(mutex_);
            if (knowledge_edits_ != 0)
            {
                return "knowledge_edit_pending";
            }
            if (!model_)
            {
                return "llm_unconfigured";
            }
            if (!speech_->connected())
            {
                return "bridge_disconnected";
            }
            if (!speech_->resolve_voice("active"))
            {
                return "unknown_voice";
            }
            session_.set_paused(false, monotonic_ms());
            changed_.notify_one();
            return {};
        }

        void begin_knowledge_edit()
        {
            const std::lock_guard lock(mutex_);
            if (knowledge_edits_ == 0)
            {
                sync_playback();
                cancel_decision();
                if (!session_.speech_id().empty())
                {
                    speech_->stop();
                    sync_playback();
                }
                session_.invalidate_conversation(monotonic_ms());
                changed_.notify_one();
            }
            ++knowledge_edits_;
        }

        void end_knowledge_edit()
        {
            const std::lock_guard lock(mutex_);
            --knowledge_edits_;
        }

        meowlive::application::SpeechStatusSnapshot stop()
        {
            std::lock_guard lock(mutex_);
            auto snapshot = speech_->stop();
            sync_playback();
            cancel_decision();
            session_.stop(monotonic_ms());
            changed_.notify_one();
            return snapshot;
        }

        meowlive::agent::Submission submit(std::vector<meowlive::domain::LiveEvent> events)
        {
            std::lock_guard lock(mutex_);
            auto counts = session_.submit(std::move(events), monotonic_ms(), wall_ms());
            changed_.notify_one();
            return counts;
        }

        meowlive::application::LiveAdmission submit_live(meowlive::domain::LiveEvent event,
                                                         std::uint64_t deadline,
                                                         meowlive::application::LiveTime received,
                                                         bool persisted)
        {
            using Admission = meowlive::application::LiveAdmission;
            const std::lock_guard lock(mutex_);
            const auto now = monotonic_ms();
            if (deadline <= now)
            {
                return Admission::Rejected;
            }
            std::vector<meowlive::domain::LiveEvent> batch;
            batch.push_back(std::move(event));
            const auto counts =
                session_.submit(std::move(batch), now, received.utc_ms, deadline, persisted);
            changed_.notify_one();
            return counts.accepted     ? Admission::Accepted
                   : counts.duplicates ? Admission::Duplicate
                                       : Admission::Rejected;
        }

        void pause_live(std::string_view source)
        {
            const std::lock_guard lock(mutex_);
            sync_playback();
            cancel_decision();
            session_.set_paused(true, monotonic_ms());
            session_.discard_pending(source);
            changed_.notify_one();
        }

        ::Json::Value snapshot();

        ::Json::Value scheduler();

        ::Json::Value activity();

        ::Json::Value traces();

        ::Json::Value trace(const std::string& id);

        ::Json::Value usage(std::string_view provider, std::string_view model, std::uint64_t since,
                            std::uint64_t until);

    private:
        void cancel_decision();

        void finish_trace(const std::string& outcome);

        void sync_playback();

        ::Json::Value environment();

        application::ModelToolResult
        invoke(const application::ModelToolCall& call,
               const std::optional<meowlive::adapters::WebSearchConfig>& search,
               const agent::DecisionWork& work, std::chrono::seconds tool_timeout,
               std::stop_token stop);

        void observe(const std::expected<application::ModelTurn, application::ModelError>& response,
                     std::chrono::system_clock::time_point started, const std::string& trace_id,
                     std::uint64_t work, bool cancelled);

        std::expected<application::AgentDecision, std::string>
        generate(const agent::DecisionWork& work, const std::string& trace_id,
                 std::stop_token stop);

        void run(std::stop_token stop);

        void commit(const agent::DecisionWork& work,
                    std::expected<application::AgentDecision, std::string> decision,
                    std::uint32_t generation);

        const std::shared_ptr<meowlive::application::SpeechWorker> speech_;
        const meowlive::application::ObsStatusQuery query_obs_;
        const std::function<bool()> live_connected_;
        const meowlive::application::ViewerMemoryQuery query_memory_;
        meowlive::server::AgentRuntimeSettings runtime_;
        std::mutex mutex_;
        std::condition_variable changed_;
        agent::AgentSession session_;
        std::optional<meowlive::adapters::ProviderConfig> model_;
        std::string provider_;
        std::string model_name_;
        std::chrono::seconds model_timeout_{30};
        unsigned max_retries_{1};
        ::Json::Value trace_;
        meowlive::server::AgentObservability observations_;
        std::deque<::Json::Value> traces_;
        std::deque<::Json::Value> usage_;
        std::uint64_t active_work_{};
        unsigned knowledge_edits_{};
        std::stop_source active_stop_;
        std::jthread worker_;
    };
}
