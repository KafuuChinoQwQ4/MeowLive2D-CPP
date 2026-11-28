module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.provider_config;

import meowlive.standard;

export namespace meowlive::adapters
{
    enum class ProviderFormat
    {
        OpenaiChat,
        OpenaiResponses,
        AnthropicMessages,
        GeminiGenerateContent,
    };

    struct ProviderConfig
    {
        meowlive::adapters::ProviderFormat format{};
        std::string base_url;
        std::string model;
        std::string api_key;
        std::chrono::seconds timeout{30};
        std::size_t max_response_bytes{1'048'576};
        std::uint32_t max_tokens{1024};
        bool json_mode{true};
        bool stream{true};
        bool cache_enabled{};
        std::string reasoning_effort;
        std::string reasoning_provider;
    };
}
