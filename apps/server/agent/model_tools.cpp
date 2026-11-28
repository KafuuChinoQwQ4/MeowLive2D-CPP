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
    ::Json::Value AgentWorker::environment()
    {
        const auto status = speech_->status();
        ::Json::Value result(::Json::objectValue);
        result["bridge_connected"] = status.connected;
        result["generation"] = status.generation;
        result["live_connected"] = live_connected_();
        result["training_busy"] = false;
        result["pending_speech"] = static_cast<unsigned>(std::ranges::count_if(
            status.tasks,
            [](const auto& task)
            {
                return task.status < meowlive::domain::SpeechStatus::Completed;
            }));
        return result;
    }

    application::ModelToolResult
    AgentWorker::invoke(const application::ModelToolCall& call,
                        const std::optional<meowlive::adapters::WebSearchConfig>& search,
                        const agent::DecisionWork& work, std::chrono::seconds tool_timeout,
                        std::stop_token stop)
    {
        ::Json::Value result(::Json::objectValue);
        const auto arguments = call.arguments_json.size() <= 4096
                                   ? meowlive::protocol::json::parse_object(call.arguments_json)
                                   : std::nullopt;
        bool failed = !arguments;
        if (!failed && call.name == "get_environment" && arguments->empty())
        {
            result = environment();
        }
        else if (!failed && call.name == "get_current_time")
        {
            const auto offset = arguments->get("utc_offset_minutes", 0);
            if (offset.isInt() && offset.asInt() >= -720 && offset.asInt() <= 840 &&
                (arguments->empty() ||
                 (arguments->size() == 1 && arguments->isMember("utc_offset_minutes"))))
            {
                const auto local =
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) +
                    std::chrono::minutes(offset.asInt());
                result["unix_time_ms"] = ::Json::UInt64(wall_ms());
                result["date_time"] = std::format("{:%FT%T}", local);
                result["utc_offset_minutes"] = offset;
            }
            else
            {
                failed = true;
            }
        }
        else if (!failed && call.name == "get_obs_status" && arguments->empty())
        {
            const auto snapshot = query_obs_(tool_timeout, stop);
            if (snapshot)
            {
                result["connected"] = snapshot->connected;
                result["recording"] = snapshot->recording;
                result["current_scene"] = snapshot->current_scene;
                result["observed_at_ms"] = ::Json::UInt64(wall_ms());
            }
            else
            {
                failed = true;
                result["error"] = "obs_unavailable";
            }
        }
        else if (!failed && query_memory_ && call.name == "get_viewer_memory" &&
                 arguments->size() == 1 && (*arguments)["event_id"].isString())
        {
            const auto id = (*arguments)["event_id"].asString();
            const auto event =
                std::ranges::find(work.request.events, id, &meowlive::domain::LiveEvent::id);
            if (event != work.request.events.end() &&
                std::ranges::find(work.memory_event_ids, id) != work.memory_event_ids.end())
            {
                const auto response = query_memory_(*event, tool_timeout, stop);
                if (response)
                {
                    return {call.id, call.name, *response, false};
                }
                result["error"] = response.error();
            }
            else
            {
                result["error"] = "viewer_memory_event_unavailable";
            }
            failed = true;
        }
        else if (!failed && search && call.name == "web_search" && arguments->size() == 1 &&
                 (*arguments)["query"].isString())
        {
            const auto response =
                meowlive::adapters::search_web(*search, (*arguments)["query"].asString(), stop);
            if (response)
            {
                return {call.id, call.name, *response, false};
            }
            result["error"] = response.error().message;
            failed = true;
        }
        else
        {
            failed = true;
        }
        if (failed && !result.isMember("error"))
        {
            result["error"] = "tool_or_arguments_unavailable";
        }
        return {call.id, call.name, meowlive::protocol::json::serialize(result), failed};
    }
}
