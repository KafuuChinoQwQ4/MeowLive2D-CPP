module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.agent_runtime_settings;

import meowlive.standard;

import meowlive.protocol.json;
import meowlive.server.profile_file;

namespace
{
    ::Json::Value defaults()
    {
        auto settings = *meowlive::protocol::json::parse_object(R"({
            "cache_enabled":false,"streaming":true,"tools_enabled":true,
            "environment_enabled":true,"web_search_enabled":false,
            "search_provider":"brave","search_endpoint":"",
            "max_tool_rounds":2,"tool_timeout_seconds":6,"prices":[]
        })");
        return settings;
    }

    bool valid_settings(const ::Json::Value& settings)
    {
        const auto model = defaults();
        if (!settings.isObject())
        {
            return false;
        }
        for (const auto& key : settings.getMemberNames())
        {
            if (!model.isMember(key) || settings[key].type() != model[key].type())
            {
                if (!model.isMember(key) || !model[key].isUInt() ||
                    !meowlive::protocol::json::is_unsigned_32(settings[key]))
                {
                    return false;
                }
            }
        }
        const auto& prices = settings["prices"];
        if (!prices.isArray() || prices.size() > 100)
        {
            return false;
        }
        for (const auto& price : prices)
        {
            if (!price.isObject() || price.size() != 7)
            {
                return false;
            }
            for (const auto* key : {"provider", "base_url", "model"})
            {
                if (!price[key].isString() || price[key].asString().size() > 512)
                {
                    return false;
                }
            }
            for (const auto* key : {"input_usd_per_million", "output_usd_per_million",
                                    "cache_read_usd_per_million", "cache_write_usd_per_million"})
            {
                if (!price[key].isNumeric() || !std::isfinite(price[key].asDouble()) ||
                    price[key].asDouble() < 0 || price[key].asDouble() > 1'000'000)
                {
                    return false;
                }
            }
        }
        const auto rounds = settings["max_tool_rounds"].asUInt();
        const auto timeout = settings["tool_timeout_seconds"].asUInt();
        return rounds >= 1 && rounds <= 3 && timeout >= 1 && timeout <= 8 &&
               (settings["search_provider"] == "brave" ||
                settings["search_provider"] == "searxng") &&
               settings["search_endpoint"].asString().size() <= 2048;
    }

    bool valid_key(const ::Json::Value& key)
    {
        return key.isNull() || (key.isString() && key.asString().size() <= 4096 &&
                                std::ranges::all_of(key.asString(),
                                                    [](unsigned char byte)
                                                    {
                                                        return byte > 32 && byte < 127;
                                                    }));
    }
}

export namespace meowlive::server
{
    enum class AgentRuntimeSaveError
    {
        Invalid,
        Storage,
    };

    class AgentRuntimeSettings
    {
    public:
        explicit AgentRuntimeSettings(std::filesystem::path path) : path_(std::move(path))
        {
            document_["schema"] = 1;
            document_["settings"] = defaults();
            std::error_code error;
            const auto present = std::filesystem::exists(path_, error);
            if (error)
            {
                throw std::runtime_error("无法访问 agent-runtime/settings.json");
            }
            if (present)
            {
                std::ifstream stream(path_, std::ios::binary);
                std::string buffer(512 * 1024 + 1, '\0');
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                buffer.resize(static_cast<std::size_t>(stream.gcount()));
                auto parsed = meowlive::protocol::json::parse_object(buffer);
                if (!stream.eof() || !parsed || (*parsed)["schema"] != 1 ||
                    !(*parsed)["settings"].isObject() || !valid_key((*parsed)["search_api_key"]))
                {
                    throw std::runtime_error("无效的 agent-runtime/settings.json");
                }
                auto& stored = (*parsed)["settings"];
                for (const auto& name : stored.getMemberNames())
                {
                    document_["settings"][name] = stored[name];
                }
                if (!valid_settings(document_["settings"]))
                {
                    throw std::runtime_error("无效的 agent-runtime/settings.json");
                }
                document_["search_api_key"] = (*parsed)["search_api_key"];
            }
        }

        ::Json::Value read() const
        {
            const std::lock_guard lock(mutex_);
            return document_;
        }

        ::Json::Value snapshot() const
        {
            auto result = read();
            result["search_key_configured"] =
                result["search_api_key"].isString() && !result["search_api_key"].asString().empty();
            result.removeMember("search_api_key");
            result.removeMember("schema");
            result["storage_available"] = true;
            return result;
        }

        std::expected<void, meowlive::server::AgentRuntimeSaveError>
        save(const ::Json::Value& input)
        {
            if (!input["settings"].isObject() || !valid_key(input["search_api_key"]) ||
                (!input["clear_search_api_key"].isNull() &&
                 !input["clear_search_api_key"].isBool()) ||
                (input.get("clear_search_api_key", false).asBool() &&
                 !input["search_api_key"].isNull()))
            {
                return std::unexpected(meowlive::server::AgentRuntimeSaveError::Invalid);
            }
            auto next = read();
            auto& settings = next["settings"];
            auto& key = next["search_api_key"];
            const auto previous_provider = settings["search_provider"];
            const auto previous_endpoint = settings["search_endpoint"];
            for (const auto& field : input["settings"].getMemberNames())
            {
                settings[field] = input["settings"][field];
            }
            if (!valid_settings(settings))
            {
                return std::unexpected(meowlive::server::AgentRuntimeSaveError::Invalid);
            }
            if (input.get("clear_search_api_key", false).asBool() ||
                previous_provider != settings["search_provider"] ||
                previous_endpoint != settings["search_endpoint"])
            {
                key = ::Json::Value{};
            }
            if (input["search_api_key"].isString())
            {
                key = input["search_api_key"];
            }
            const auto temporary =
                path_.parent_path() / ("settings-" + drogon::utils::getUuid() + ".tmp");
            if (!meowlive::server::write_profile_file(path_, temporary,
                                                      meowlive::protocol::json::serialize(next)))
            {
                return std::unexpected(meowlive::server::AgentRuntimeSaveError::Storage);
            }
            const std::lock_guard lock(mutex_);
            document_ = std::move(next);
            return {};
        }

    private:
        std::filesystem::path path_;
        mutable std::mutex mutex_;
        ::Json::Value document_;
    };
}
