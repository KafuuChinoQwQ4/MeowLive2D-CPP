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
    ::Json::Value AgentWorker::snapshot()
    {
        std::lock_guard lock(mutex_);
        sync_playback();
        session_.expire(monotonic_ms());
        ::Json::Value status(::Json::objectValue);
        status["phase"] = phase_name(session_.phase());
        status["paused"] = session_.phase() == agent::AgentPhase::Paused;
        status["settings"] = json::settings(session_.settings());
        status["bridge_connected"] = speech_->connected();
        status["llm_configured"] = static_cast<bool>(model_);
        status["last_error"] = optional_text(session_.last_error());
        status["current_speech_id"] = optional_text(session_.speech_id());
        status["events"] = ::Json::Value(::Json::arrayValue);
        for (const auto& record : session_.events())
        {
            ::Json::Value item(::Json::objectValue);
            item["event"] = json::event(record.event);
            item["status"] = std::string{meowlive::agent::event_status_name(record.status)};
            item["speech_id"] = optional_text(record.speech_id);
            item["error"] = optional_text(record.error);
            status["events"].append(std::move(item));
        }
        status["history"] = ::Json::Value(::Json::arrayValue);
        for (const auto& turn : session_.history())
        {
            ::Json::Value item(::Json::objectValue);
            item["user"] = turn.user;
            item["assistant"] = turn.assistant;
            status["history"].append(std::move(item));
        }
        return status;
    }

    ::Json::Value AgentWorker::scheduler()
    {
        std::lock_guard lock(mutex_);
        sync_playback();
        session_.expire(monotonic_ms());
        const auto now = monotonic_ms();
        const auto phase = session_.phase();
        ::Json::Value result(::Json::objectValue);
        result["phase"] = phase_name(phase);
        result["bridge_connected"] = speech_->connected();
        result["llm_configured"] = static_cast<bool>(model_);
        result["reason"] = phase == agent::AgentPhase::Paused     ? "paused"
                           : !speech_->connected()                ? "bridge_disconnected"
                           : !model_                              ? "llm_unconfigured"
                           : phase == agent::AgentPhase::Deciding ? "decision_in_flight"
                           : phase == agent::AgentPhase::Speaking ? "speech_in_flight"
                           : session_.cooldown_until() > now      ? "cooldown"
                                                                  : "no_eligible_events";
        result["cooldown_remaining_ms"] =
            ::Json::UInt64(session_.cooldown_until() > now ? session_.cooldown_until() - now : 0);
        result["events"] = ::Json::Value(::Json::objectValue);
        for (const auto& row : session_.events())
        {
            auto& count =
                result["events"][std::string{meowlive::agent::event_status_name(row.status)}];
            count = count.asUInt() + 1;
        }
        return result;
    }

    ::Json::Value AgentWorker::activity()
    {
        std::lock_guard lock(mutex_);
        sync_playback();
        ::Json::Value result(::Json::objectValue);
        result["phase"] = phase_name(session_.phase());
        result["current"] = trace_;
        result["recent"] = traces_.empty() ? ::Json::Value{} : traces_.back();
        return result;
    }

    ::Json::Value AgentWorker::traces()
    {
        std::lock_guard lock(mutex_);
        sync_playback();
        ::Json::Value result(::Json::objectValue);
        result["active"] = trace_;
        result["traces"] = ::Json::Value(::Json::arrayValue);
        for (auto it = traces_.rbegin(); it != traces_.rend(); ++it)
        {
            result["traces"].append(*it);
        }
        return result;
    }

    ::Json::Value AgentWorker::trace(const std::string& id)
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : traces_)
        {
            if (item["id"] == id)
            {
                return item;
            }
        }
        return !trace_.isNull() && trace_["id"] == id ? trace_ : ::Json::Value{};
    }

    ::Json::Value AgentWorker::usage(std::string_view provider, std::string_view model,
                                     std::uint64_t since, std::uint64_t until)
    {
        std::lock_guard lock(mutex_);
        ::Json::Value result(::Json::objectValue);
        result["records"] = ::Json::Value(::Json::arrayValue);
        application::TokenUsage total;
        const auto prices = runtime_.read()["settings"]["prices"];
        double estimated{};
        bool priced{};
        for (auto it = usage_.rbegin(); it != usage_.rend(); ++it)
        {
            const auto at = (*it)["started_at_ms"].asUInt64();
            if ((!provider.empty() && (*it)["provider"].asString() != provider) ||
                (!model.empty() && (*it)["model"].asString() != model) || at < since || at > until)
            {
                continue;
            }
            auto record = *it;
            for (const auto& price : prices)
            {
                if (price["provider"] != record["provider"] || price["model"] != record["model"] ||
                    meowlive::server::model_destination_key(price["base_url"].asString()) !=
                        record["base_url_hash"].asString() ||
                    !record["input_tokens"].isUInt64() || !record["output_tokens"].isUInt64())
                {
                    continue;
                }
                const auto input_price = price["input_usd_per_million"].asDouble();
                const auto cache_read_price = price["cache_read_usd_per_million"].asDouble();
                const auto cache_write_price = price["cache_write_usd_per_million"].asDouble();
                const auto& read = record["cache_read_tokens"];
                const auto& write = record["cache_write_tokens"];
                if ((read.isNull() && input_price != cache_read_price) ||
                    (write.isNull() && input_price != cache_write_price) ||
                    (record["total_tokens"].asUInt64() == 0 && record["state"] != "completed"))
                {
                    break;
                }
                const auto inputs = record["input_tokens"].asUInt64();
                const auto read_tokens = (std::min)(read.asUInt64(), inputs);
                const auto write_tokens = (std::min)(write.asUInt64(), inputs - read_tokens);
                const auto usd =
                    ((inputs - read_tokens - write_tokens) * input_price +
                     read_tokens * cache_read_price + write_tokens * cache_write_price +
                     record["output_tokens"].asDouble() *
                         price["output_usd_per_million"].asDouble()) /
                    1'000'000;
                record["estimated_cost_usd"] = usd;
                estimated += usd;
                priced = true;
                break;
            }
            record.removeMember("base_url_hash");
            result["records"].append(std::move(record));
            total.input += (*it)["input_tokens"].asUInt64();
            total.output += (*it)["output_tokens"].asUInt64();
            total.total += (*it)["total_tokens"].asUInt64();
        }
        result["input_tokens"] = ::Json::UInt64(total.input);
        result["output_tokens"] = ::Json::UInt64(total.output);
        result["total_tokens"] = ::Json::UInt64(total.total);
        result["estimated_cost_usd"] = priced ? ::Json::Value(estimated) : ::Json::Value{};
        return result;
    }
}
