module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.agent_observability;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.profile_file;

namespace
{
    constexpr std::size_t trace_count = 100;
    constexpr std::size_t turn_count = 200;
    constexpr std::size_t file_limit = 2 * 1024 * 1024;

    ::Json::Value fields(const ::Json::Value& record, std::initializer_list<const char*> names)
    {
        ::Json::Value result(::Json::objectValue);
        for (const auto* name : names)
        {
            if (record.isMember(name) && !record[name].isObject() && !record[name].isArray() &&
                (!record[name].isString() || record[name].asString().size() <= 256))
            {
                result[name] = record[name];
            }
        }
        return result;
    }

    ::Json::Value safe_turn(const ::Json::Value& record)
    {
        auto result = fields(record, {"trace_id", "provider", "model", "started_at_ms",
                                      "input_tokens", "output_tokens", "total_tokens",
                                      "base_url_hash", "cache_read_tokens", "cache_write_tokens",
                                      "reasoning_tokens", "first_response_ms", "state"});
        for (const auto* field : {"started_at_ms", "input_tokens", "output_tokens", "total_tokens"})
        {
            if (!result[field].isUInt64())
            {
                result[field] = ::Json::UInt64{0};
            }
        }
        for (const auto* field : {"trace_id", "provider", "model"})
        {
            if (!result[field].isString())
            {
                result[field] = "";
            }
        }
        result["tools"] = ::Json::Value(::Json::arrayValue);
        if (record["tools"].isArray())
        {
            for (const auto& tool : record["tools"])
            {
                if (result["tools"].size() == 32)
                {
                    break;
                }
                if (tool == "get_environment" || tool == "get_current_time" ||
                    tool == "web_search" || tool == "get_obs_status")
                {
                    result["tools"].append(tool);
                }
                else
                {
                    result["tools"].append("unavailable");
                }
            }
        }
        return result;
    }

    ::Json::Value safe_trace(const ::Json::Value& record)
    {
        auto result =
            fields(record, {"id", "state", "created_at_ms", "finished_at_ms", "speech_id"});
        result["event_ids"] = ::Json::Value(::Json::arrayValue);
        if (record["event_ids"].isArray())
        {
            for (const auto& id : record["event_ids"])
            {
                if (result["event_ids"].size() == 32)
                {
                    break;
                }
                if (id.isString() && id.asString().size() <= 128)
                {
                    result["event_ids"].append(id);
                }
            }
        }
        result["turns"] = ::Json::Value(::Json::arrayValue);
        if (record["turns"].isArray())
        {
            for (const auto& turn : record["turns"])
            {
                if (result["turns"].size() == 16)
                {
                    break;
                }
                if (turn.isObject())
                {
                    result["turns"].append(safe_turn(turn));
                }
            }
        }
        return result;
    }

    using Redact = ::Json::Value (*)(const ::Json::Value&);

    std::deque<::Json::Value> read_recent(const std::filesystem::path& path, std::size_t limit,
                                          Redact redact)
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found))
        {
            return {};
        }
        if (error || !std::filesystem::is_regular_file(status) ||
            std::filesystem::file_size(path, error) > file_limit || error)
        {
            LOG_WARN << "agent_observability_read_failed";
            return {};
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            LOG_WARN << "agent_observability_read_failed";
            return {};
        }
        std::deque<::Json::Value> result;
        for (std::string line; std::getline(stream, line);)
        {
            if (auto record = meowlive::protocol::json::parse_object(line))
            {
                result.push_back(redact(*record));
                if (result.size() > limit)
                {
                    result.pop_front();
                }
            }
        }
        return result;
    }

    bool write_recent(const std::filesystem::path& path, const std::deque<::Json::Value>& records)
    {
        std::string bytes;
        for (const auto& record : records)
        {
            bytes += meowlive::protocol::json::serialize(record);
            bytes += '\n';
        }
        if (bytes.size() > file_limit)
        {
            const auto first = bytes.find('\n', bytes.size() - file_limit);
            bytes.erase(0, first + 1);
        }
        const auto temporary =
            path.parent_path() / (".agent-observation-" + drogon::utils::getUuid() + ".tmp");
        if (!meowlive::server::write_profile_file(path, temporary, bytes))
        {
            LOG_WARN << "agent_observability_write_failed";
            return false;
        }
        return true;
    }

    std::deque<::Json::Value> restore_traces(const std::filesystem::path& terminal,
                                             const std::filesystem::path& active)
    {
        auto result = read_recent(terminal, trace_count, safe_trace);
        auto unfinished = read_recent(active, 1, safe_trace);
        if (unfinished.empty())
        {
            return result;
        }
        auto& trace = unfinished.front();
        if (!trace["id"].isString())
        {
            return result;
        }
        const bool finalized = std::ranges::any_of(result,
                                                   [&](const auto& record)
                                                   {
                                                       return record["id"] == trace["id"];
                                                   });
        if (!finalized)
        {
            trace["state"] = "unknown";
            trace["finished_at_ms"] =
                static_cast<::Json::UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
            result.push_back(trace);
            if (result.size() > trace_count)
            {
                result.pop_front();
            }
            if (!write_recent(terminal, result))
            {
                return result;
            }
        }
        write_recent(active, {});
        return result;
    }
}

export namespace meowlive::server
{
    std::string model_destination_key(std::string_view base)
    {
        return drogon::utils::getSha256(base.data(), base.size());
    }

    class AgentObservability
    {
    public:
        explicit AgentObservability(const std::filesystem::path& directory)
            : trace_file_(directory / "traces.jsonl"), turn_file_(directory / "turns.jsonl"),
              active_file_(directory / "active-trace.jsonl"),
              traces_(restore_traces(trace_file_, active_file_)),
              turns_(read_recent(turn_file_, turn_count, safe_turn)), startup_traces_(traces_),
              startup_turns_(turns_)
        {
        }

        const std::deque<::Json::Value>& loaded_traces() const
        {
            return startup_traces_;
        }

        const std::deque<::Json::Value>& loaded_turns() const
        {
            return startup_turns_;
        }

        void trace(::Json::Value value)
        {
            enqueue(std::move(value), true);
        }

        void turn(::Json::Value value)
        {
            enqueue(std::move(value), false);
        }

        void active(::Json::Value value)
        {
            const std::scoped_lock lock(active_mutex_);
            pending_active_ = std::move(value);
            if (active_scheduled_)
            {
                return;
            }
            const auto status = writes_.try_submit(
                [this]() noexcept
                {
                    try
                    {
                        ::Json::Value pending;
                        {
                            const std::scoped_lock lock(active_mutex_);
                            pending = std::exchange(pending_active_, ::Json::Value{});
                            active_scheduled_ = false;
                        }
                        if (pending.isObject() && pending["id"].isString())
                        {
                            const auto record = safe_trace(pending);
                            if (write_recent(active_file_, {record}))
                            {
                                active_id_ = record["id"].asString();
                            }
                        }
                    }
                    catch (...)
                    {
                        meowlive::platform::log_current_exception(
                            "agent.observability.write_active",
                            meowlive::platform::ExceptionDetails::Omit);
                    }
                });
            active_scheduled_ = status == meowlive::application::WorkSubmission::Accepted;
            if (!active_scheduled_)
            {
                LOG_WARN << "agent_observability_queue_unavailable";
            }
        }

    private:
        void enqueue(::Json::Value value, bool is_trace)
        {
            const auto submitted = writes_.try_submit(
                [this, value = std::move(value), is_trace]() noexcept
                {
                    try
                    {
                        auto& records = is_trace ? traces_ : turns_;
                        records.push_back(is_trace ? safe_trace(value) : safe_turn(value));
                        if (records.size() > (is_trace ? trace_count : turn_count))
                        {
                            records.pop_front();
                        }
                        if (write_recent(is_trace ? trace_file_ : turn_file_, records) &&
                            is_trace && value["id"].isString() &&
                            value["id"].asString() == active_id_ && write_recent(active_file_, {}))
                        {
                            active_id_.clear();
                        }
                    }
                    catch (...)
                    {
                        meowlive::platform::log_current_exception(
                            is_trace ? "agent.observability.write_trace"
                                     : "agent.observability.write_turn",
                            meowlive::platform::ExceptionDetails::Omit);
                    }
                });
            if (submitted != meowlive::application::WorkSubmission::Accepted)
            {
                LOG_WARN << "agent_observability_queue_unavailable";
            }
        }

        const std::filesystem::path trace_file_;
        const std::filesystem::path turn_file_;
        const std::filesystem::path active_file_;
        std::deque<::Json::Value> traces_;
        std::deque<::Json::Value> turns_;
        const std::deque<::Json::Value> startup_traces_;
        const std::deque<::Json::Value> startup_turns_;
        std::mutex active_mutex_;
        ::Json::Value pending_active_;
        bool active_scheduled_{};
        std::string active_id_;
        meowlive::application::WorkQueue writes_{128};
    };
}
