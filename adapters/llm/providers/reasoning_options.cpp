module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.adapters.llm.request;

import meowlive.standard;
import meowlive.adapters.llm.provider_config;
import meowlive.application.llm;
import meowlive.domain.live_event;
import meowlive.protocol.json;

namespace meowlive::adapters::request_detail
{
    bool apply_reasoning(Json::Value& body, const adapters::ProviderConfig& config)
    {
        using enum adapters::ProviderFormat;
        const auto& requested = config.reasoning_effort;
        if (requested.empty() || requested == "default")
        {
            return true;
        }
        constexpr std::array<std::string_view, 7> levels{"minimal", "low", "medium", "high",
                                                         "xhigh",   "max", "ultra"};
        const auto requested_level = std::ranges::find(levels, requested);
        if (requested_level == levels.end())
        {
            return false;
        }
        const auto level = static_cast<std::size_t>(requested_level - levels.begin());
        std::string_view model(config.model);
        if (model.starts_with("models/"))
        {
            model.remove_prefix(7);
        }
        const auto& provider = config.reasoning_provider;
        const bool infer = provider.empty() || provider == "custom";
        const auto select = [&](std::initializer_list<std::size_t> supported)
        {
            auto selected = *supported.begin();
            for (const auto candidate : supported)
            {
                if (candidate <= level)
                {
                    selected = candidate;
                }
            }
            return std::string(levels[selected]);
        };
        const auto budget = [&](std::uint32_t minimum,
                                std::uint32_t desired) -> std::optional<std::uint32_t>
        {
            if (config.max_tokens < minimum + 512)
            {
                return std::nullopt;
            }
            return std::min(desired, config.max_tokens - 512);
        };

        if ((infer || provider == "openai") &&
            (config.format == OpenaiChat || config.format == OpenaiResponses) &&
            (model.starts_with("gpt-5") || model == "o1" || model.starts_with("o1-2") ||
             model == "o3" || model.starts_with("o3-2") || model.starts_with("o4-mini")))
        {
            const auto effort =
                model.starts_with("gpt-5.6") ? select({1, 2, 3, 4, 5})
                : model.starts_with("gpt-5.2") || model.starts_with("gpt-5.4") || model == "gpt-5.5"
                    ? select({1, 2, 3, 4})
                : model.starts_with("gpt-5") && !model.starts_with("gpt-5.1") ? select({0, 1, 2, 3})
                                                                              : select({1, 2, 3});
            if (config.format == OpenaiChat)
            {
                body["reasoning_effort"] = effort;
            }
            else
            {
                body["reasoning"]["effort"] = effort;
            }
        }
        else if ((infer || provider == "deepseek") && model.starts_with("deepseek-v4"))
        {
            const auto effort = select({1, 3, 5});
            if (config.format == OpenaiChat)
            {
                body["thinking"]["type"] = "enabled";
                body["reasoning_effort"] = effort;
            }
            else if (config.format == OpenaiResponses)
            {
                body["reasoning"]["effort"] = effort;
            }
            else if (config.format == AnthropicMessages)
            {
                body["thinking"]["type"] = "enabled";
                body["output_config"]["effort"] = effort;
            }
        }
        else if ((infer || provider == "kimi") && model == "kimi-k3" && config.format == OpenaiChat)
        {
            body["reasoning_effort"] = select({1, 3, 5});
        }
        else if ((infer || provider == "xai" || provider == "grok") &&
                 (model == "grok-4.5" || model == "grok-4.6" || model == "grok-4.7"))
        {
            const auto effort = model == "grok-4.5" ? select({1, 2, 3}) : select({1, 2, 3, 4});
            if (config.format == OpenaiChat)
            {
                body["reasoning_effort"] = effort;
            }
            else if (config.format == OpenaiResponses)
            {
                body["reasoning"]["effort"] = effort;
            }
        }
        else if ((infer || provider == "anthropic" || provider == "claude") &&
                 config.format == AnthropicMessages)
        {
            if (model == "claude-opus-4-6" || model == "claude-sonnet-4-6" ||
                model == "claude-opus-4-7" || model == "claude-opus-4-8" ||
                model == "claude-opus-5" || model == "claude-sonnet-5")
            {
                body["thinking"]["type"] = "adaptive";
                body["output_config"]["effort"] =
                    model.ends_with("-4-6") ? select({1, 2, 3, 5}) : select({1, 2, 3, 4, 5});
            }
            else if (model.starts_with("claude-opus-4-5") ||
                     model.starts_with("claude-sonnet-4-5") ||
                     model.starts_with("claude-haiku-4-5") ||
                     model.starts_with("claude-sonnet-4-0") ||
                     model.starts_with("claude-opus-4-0") || model.starts_with("claude-opus-4-1"))
            {
                constexpr std::array<std::uint32_t, 7> preset{1024,  2048,  4096, 8192,
                                                              16384, 24576, 32768};
                const auto allowed = budget(1024, preset[level]);
                if (!allowed)
                {
                    return false;
                }
                body["thinking"]["type"] = "enabled";
                body["thinking"]["budget_tokens"] = *allowed;
            }
        }
        else if ((infer || provider == "gemini") && config.format == GeminiGenerateContent)
        {
            if (model.starts_with("gemini-3") && (model.find("flash") != std::string_view::npos ||
                                                  model.find("pro") != std::string_view::npos))
            {
                auto effort = model == "gemini-3-pro-preview"               ? select({1, 3})
                              : model.find("pro") != std::string_view::npos ? select({1, 2, 3})
                                                                            : select({0, 1, 2, 3});
                std::ranges::transform(effort, effort.begin(),
                                       [](char ch)
                                       {
                                           return static_cast<char>(ch - 'a' + 'A');
                                       });
                body["generationConfig"]["thinkingConfig"]["thinkingLevel"] = effort;
            }
            else if (model == "gemini-2.5-pro" || model == "gemini-2.5-flash" ||
                     model == "gemini-2.5-flash-lite")
            {
                constexpr std::array<std::uint32_t, 7> preset{512,  1024,  2048, 4096,
                                                              8192, 16384, 24576};
                const std::uint32_t minimum = model == "gemini-2.5-pro"          ? 128
                                              : model == "gemini-2.5-flash-lite" ? 512
                                                                                 : 1;
                const auto allowed = budget(minimum, preset[level]);
                if (!allowed)
                {
                    return false;
                }
                body["generationConfig"]["thinkingConfig"]["thinkingBudget"] = *allowed;
            }
        }
        return true;
    }
}
