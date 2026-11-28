module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.agent_model;

import meowlive.standard;

import meowlive.adapters.llm.language_model;

export namespace meowlive::server
{
    std::optional<meowlive::adapters::ProviderConfig> provider_config(const ::Json::Value& input)
    {
        if (!input.isObject() || !input["base_url"].isString() ||
            input["base_url"].asString().empty())
        {
            return {};
        }
        using Format = meowlive::adapters::ProviderFormat;
        const auto format = input.get("api_format", "openai_chat").asString();
        meowlive::adapters::ProviderConfig result;
        if (format == "openai_chat")
        {
            result.format = Format::OpenaiChat;
        }
        else if (format == "openai_responses")
        {
            result.format = Format::OpenaiResponses;
        }
        else if (format == "anthropic_messages")
        {
            result.format = Format::AnthropicMessages;
        }
        else if (format == "gemini_generate_content")
        {
            result.format = Format::GeminiGenerateContent;
        }
        else
        {
            return {};
        }
        result.base_url = input["base_url"].asString();
        result.model = input.get("model", "").asString();
        result.api_key = input["api_key"].isString() ? input["api_key"].asString() : "";
        const auto key_variable = input.get("api_key_env", "MEOWLIVE_LLM_API_KEY").asString();
        if (result.api_key.empty() && !key_variable.empty())
        {
            if (const char* value = std::getenv(key_variable.c_str()))
            {
                result.api_key = value;
            }
        }
        result.timeout = std::chrono::seconds(input.get("timeout_seconds", 30).asUInt());
        result.max_response_bytes = input.get("max_response_bytes", 65'536).asUInt();
        result.max_tokens = input.get("max_tokens", 1024).asUInt();
        result.json_mode = input.get("json_mode", true).asBool();
        result.stream = input.get("stream", true).asBool();
        result.reasoning_effort = input.get("reasoning_effort", "default").asString();
        return result;
    }
}
