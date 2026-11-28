module;

#include <json/json.h>
#include <toml++/toml.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.profile_defaults;

import meowlive.standard;

namespace
{
    void apply_toml(::Json::Value& defaults, toml::node_view<const toml::node> section)
    {
        if (!section)
        {
            return;
        }
        if (!section.is_table())
        {
            throw std::runtime_error("配置无效 [agent/llm]");
        }
        for (const auto& name : defaults.getMemberNames())
        {
            const auto value = section[name];
            if (!value)
            {
                continue;
            }
            auto& target = defaults[name];
            if (target.isObject())
            {
                apply_toml(target, value);
            }
            else if (target.isString() && value.is_string())
            {
                target = *value.value_exact<std::string>();
            }
            else if (target.isBool() && value.is_boolean())
            {
                target = *value.value_exact<bool>();
            }
            else if (target.isUInt() && value.is_integer() &&
                     *value.value_exact<std::int64_t>() >= 0)
            {
                target = static_cast<::Json::UInt64>(*value.value_exact<std::int64_t>());
            }
            else
            {
                throw std::runtime_error("配置无效 [agent/llm." + name + "]");
            }
        }
    }
}

export namespace meowlive::server
{
    struct ProfileDefaults
    {
        ::Json::Value agent;
        ::Json::Value llm;
    };

    meowlive::server::ProfileDefaults profile_defaults(const std::filesystem::path& config)
    {
        meowlive::server::ProfileDefaults defaults;
        auto& agent = defaults.agent;
        agent["persona"] = "你是一位友好、自然的 Live2D 主播，用简短中文与观众互动。";
        agent["system_prompt"] = "";
        agent["topic"] = "";
        agent["proactive_enabled"] = false;
        agent["cooldown_ms"] = 30'000;
        auto& interaction = agent["interaction"];
        interaction["chat_read_mode"] = "auto";
        interaction["welcome_enabled"] = true;
        interaction["busy_chat_count"] = 6;
        interaction["busy_enter_count"] = 3;
        interaction["busy_pending_count"] = 4;
        interaction["welcome_cooldown_ms"] = 30'000;
        interaction["welcome_viewer_cooldown_ms"] = 600'000;

        auto& llm = defaults.llm;
        llm["mode"] = "cloud";
        llm["provider"] = "custom";
        llm["api_format"] = "openai_chat";
        llm["base_url"] = "";
        llm["model"] = "";
        llm["api_key_env"] = "MEOWLIVE_LLM_API_KEY";
        llm["timeout_seconds"] = 30;
        llm["max_response_bytes"] = 65'536;
        llm["max_tokens"] = 1024;
        llm["json_mode"] = true;
        llm["reasoning_effort"] = "default";
        llm["max_retries"] = 1;

        std::ifstream input(config, std::ios::binary);
        if (input)
        {
            try
            {
                const auto document = toml::parse(input);
                apply_toml(agent, document["agent"]);
                apply_toml(llm, document["llm"]);
            }
            catch (const toml::parse_error& error)
            {
                throw std::runtime_error("配置无效 [server.toml 行 " +
                                         std::to_string(error.source().begin.line) + "]");
            }
        }
        return defaults;
    }
}
