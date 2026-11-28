module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.agent_worker;

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
    void AgentWorker::observe(
        const std::expected<application::ModelTurn, application::ModelError>& response,
        std::chrono::system_clock::time_point started, const std::string& trace_id,
        std::uint64_t work, bool cancelled)
    {
        std::lock_guard lock(mutex_);
        ::Json::Value entry(::Json::objectValue);
        entry["trace_id"] = trace_id;
        entry["provider"] = provider_;
        entry["model"] = model_name_;
        entry["base_url_hash"] = meowlive::server::model_destination_key(model_->base_url);
        entry["started_at_ms"] = ::Json::UInt64(
            std::chrono::duration_cast<std::chrono::milliseconds>(started.time_since_epoch())
                .count());
        const application::ModelTurn empty;
        const auto& turn = response ? *response : empty;
        entry["input_tokens"] = ::Json::UInt64(turn.usage.input);
        entry["output_tokens"] = ::Json::UInt64(turn.usage.output);
        entry["total_tokens"] = ::Json::UInt64(turn.usage.total);
        const auto token_count = [](const std::optional<std::uint64_t>& value)
        {
            return value ? ::Json::Value(::Json::UInt64(*value)) : ::Json::Value{};
        };
        entry["cache_read_tokens"] = token_count(turn.usage.cache_read);
        entry["cache_write_tokens"] = token_count(turn.usage.cache_write);
        entry["reasoning_tokens"] = token_count(turn.usage.reasoning);
        entry["first_response_ms"] = token_count(turn.first_response_ms);
        entry["state"] = cancelled ? "cancelled" : response ? "completed" : "failed";
        entry["tools"] = ::Json::Value(::Json::arrayValue);
        for (const auto& call : turn.tool_calls)
        {
            entry["tools"].append(
                call.name == "get_environment" || call.name == "get_current_time" ||
                        call.name == "web_search" || call.name == "get_obs_status" ||
                        call.name == "get_viewer_memory"
                    ? call.name
                    : "unavailable");
        }
        if (!trace_.isNull() && active_work_ == work)
        {
            auto trace_entry = entry;
            trace_entry.removeMember("base_url_hash");
            trace_["turns"].append(std::move(trace_entry));
            observations_.active(trace_);
        }
        observations_.turn(entry);
        usage_.push_back(std::move(entry));
        if (usage_.size() > 200)
        {
            usage_.pop_front();
        }
    }

    void AgentWorker::cancel_decision()
    {
        active_stop_.request_stop();
        if (!trace_.isNull() && trace_["state"] == "deciding")
        {
            finish_trace("cancelled");
        }
    }

    void AgentWorker::finish_trace(const std::string& outcome)
    {
        if (trace_.isNull())
        {
            return;
        }
        trace_["state"] = outcome;
        trace_["finished_at_ms"] = ::Json::UInt64(wall_ms());
        observations_.trace(trace_);
        traces_.push_back(std::exchange(trace_, ::Json::Value{}));
        if (traces_.size() > 100)
        {
            traces_.pop_front();
        }
    }

    void AgentWorker::sync_playback()
    {
        const auto task = speech_->agent_playback();
        if (!task || session_.speech_id() != task->id)
        {
            return;
        }
        session_.sync(*task, monotonic_ms());
        switch (task->status)
        {
        case meowlive::domain::SpeechStatus::Completed:
            finish_trace("completed");
            break;
        case meowlive::domain::SpeechStatus::Cancelled:
            finish_trace("cancelled");
            break;
        case meowlive::domain::SpeechStatus::Failed:
            finish_trace("failed");
            break;
        case meowlive::domain::SpeechStatus::Unknown:
            finish_trace("unknown");
            break;
        default:
            break;
        }
    }
}
