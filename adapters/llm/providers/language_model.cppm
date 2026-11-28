module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.language_model;

import meowlive.standard;
export import meowlive.adapters.llm.provider_config;
import meowlive.adapters.llm.http;
import meowlive.adapters.llm.request;
import meowlive.adapters.llm.output;
import meowlive.adapters.llm.stream;
import meowlive.application.llm;
import meowlive.protocol.json;

namespace
{
    namespace app = meowlive::application;
    namespace llm = meowlive::adapters::llm_detail;
    using meowlive::adapters::ProviderConfig;
    using meowlive::adapters::ProviderFormat;

    std::expected<std::string, app::ModelError> model_base(const ProviderConfig& config)
    {
        auto base = llm::validate_model_url(config.base_url);
        if (!base)
        {
            return base;
        }
        const auto authority = base->find("://") + 3;
        const auto path = base->find('/', authority);
        if (path == std::string::npos)
        {
            return *base +
                   (config.format == ProviderFormat::GeminiGenerateContent ? "/v1beta" : "/v1");
        }
        constexpr std::string_view endings[]{"/chat/completions", "/responses", "/messages",
                                             "/models"};
        for (const auto ending : endings)
        {
            if (std::string_view(*base).substr(path).ends_with(ending))
            {
                base->resize(base->size() - ending.size());
                return base;
            }
        }
        const auto models = base->find("/models/", path);
        if (models != std::string::npos &&
            (base->ends_with(":generateContent") || base->ends_with(":streamGenerateContent")))
        {
            base->resize(models);
        }
        return base;
    }

    std::expected<std::vector<std::string>, app::ModelError>
    model_headers(const ProviderConfig& config)
    {
        if (config.api_key.size() > 4096 ||
            std::ranges::any_of(config.api_key,
                                [](unsigned char ch)
                                {
                                    return ch <= 32 || ch >= 127;
                                }) ||
            config.timeout.count() <= 0 || config.timeout.count() > 300 ||
            config.max_response_bytes < 1024 || config.max_response_bytes > 8 * 1024 * 1024 ||
            config.max_tokens < 16 || config.max_tokens > 65536)
        {
            return std::unexpected(app::ModelError{"模型服务配置无效"});
        }
        std::vector<std::string> headers{"Content-Type: application/json",
                                         "Accept: application/json, text/event-stream"};
        if (config.format == ProviderFormat::AnthropicMessages)
        {
            headers.emplace_back("anthropic-version: 2023-06-01");
        }
        if (!config.api_key.empty())
        {
            switch (config.format)
            {
            case ProviderFormat::OpenaiChat:
            case ProviderFormat::OpenaiResponses:
                headers.push_back("Authorization: Bearer " + config.api_key);
                break;
            case ProviderFormat::AnthropicMessages:
                headers.push_back("x-api-key: " + config.api_key);
                break;
            case ProviderFormat::GeminiGenerateContent:
                headers.push_back("x-goog-api-key: " + config.api_key);
                break;
            }
        }
        return headers;
    }

    std::expected<std::string, app::ModelError> completion_url(const ProviderConfig& config,
                                                               const std::string& base)
    {
        const app::ModelError invalid{"模型 ID 无效"};
        if (config.model.empty() || config.model.size() > 4096)
        {
            return std::unexpected(invalid);
        }
        switch (config.format)
        {
        case ProviderFormat::OpenaiChat:
            return base + "/chat/completions";
        case ProviderFormat::OpenaiResponses:
            return base + "/responses";
        case ProviderFormat::AnthropicMessages:
            return base + "/messages";
        case ProviderFormat::GeminiGenerateContent:
        {
            std::string_view model(config.model);
            if (model.starts_with("models/"))
            {
                model.remove_prefix(7);
            }
            if (model.empty() || !std::ranges::all_of(model,
                                                      [](unsigned char ch)
                                                      {
                                                          return (ch >= 'a' && ch <= 'z') ||
                                                                 (ch >= 'A' && ch <= 'Z') ||
                                                                 (ch >= '0' && ch <= '9') ||
                                                                 ch == '_' || ch == '-' ||
                                                                 ch == '.';
                                                      }))
            {
                return std::unexpected(invalid);
            }
            return base + "/models/" + std::string(model) +
                   (config.stream ? ":streamGenerateContent?alt=sse" : ":generateContent");
        }
        }
        return std::unexpected(invalid);
    }

    std::string encode_query(std::string_view input)
    {
        constexpr std::string_view digits = "0123456789ABCDEF";
        std::string result;
        for (const auto value : input)
        {
            const auto byte = static_cast<unsigned char>(value);
            result += '%';
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
        return result;
    }
}

export namespace meowlive::adapters
{
    std::string provider_reasoning_preview(const meowlive::adapters::ProviderConfig& config)
    {
        return llm::reasoning_preview(config);
    }

    class ProviderModel final : public app::LanguageModel
    {
    public:
        explicit ProviderModel(meowlive::adapters::ProviderConfig config)
            : config_(std::move(config))
        {
        }

        std::expected<app::ModelTurn, app::ModelError> turn(const app::DecisionRequest& request,
                                                            const app::ModelOptions& options,
                                                            std::stop_token stop) override
        {
            const auto base = model_base(config_);
            const auto headers = model_headers(config_);
            if (!base || !headers)
            {
                return std::unexpected(base ? headers.error() : base.error());
            }
            const auto url = completion_url(config_, *base);
            if (!url)
            {
                return std::unexpected(url.error());
            }
            auto body = llm::model_request(config_, request, options);
            if (!body)
            {
                return std::unexpected(body.error());
            }
            const auto response =
                llm::model_http(*url, *headers, &*body, config_.timeout,
                                config_.stream ? std::min(config_.max_response_bytes * 8,
                                                          std::size_t{8 * 1024 * 1024})
                                               : config_.max_response_bytes,
                                stop);
            if (!response)
            {
                return std::unexpected(response.error());
            }
            const auto raw = response->content_type.starts_with("text/event-stream")
                                 ? llm::parse_sse(config_.format, response->body)
                                 : meowlive::protocol::json::parse_object(response->body);
            if (!raw ||
                meowlive::protocol::json::serialize(*raw).size() > config_.max_response_bytes)
            {
                return std::unexpected(app::ModelError{"模型响应缺失、格式无效或超过大小上限"});
            }
            auto output = llm::model_output(config_, *raw, options);
            if (output)
            {
                output->first_response_ms = response->first_response_ms;
            }
            return output;
        }

        std::expected<std::string, app::ModelError> complete_json(std::string_view system,
                                                                  std::string_view input,
                                                                  std::stop_token stop) override
        {
            auto config = config_;
            config.stream = false;
            config.json_mode = true;
            constexpr std::size_t limit = 64 * 1024;
            config.max_response_bytes = std::min(config.max_response_bytes, limit);
            const auto base = model_base(config);
            const auto headers = model_headers(config);
            if (!base || !headers)
            {
                return std::unexpected(base ? headers.error() : base.error());
            }
            const auto url = completion_url(config, *base);
            const auto body = llm::completion_request(config, system, input);
            if (!url || !body)
            {
                return std::unexpected(url ? body.error() : url.error());
            }
            if (body->size() > limit)
            {
                return std::unexpected(app::ModelError{"模型结构化补全请求超过大小上限"});
            }
            const auto response = llm::model_http(*url, *headers, &*body, config.timeout,
                                                  config.max_response_bytes, stop);
            if (!response)
            {
                return std::unexpected(response.error());
            }
            if (const auto raw = meowlive::protocol::json::parse_object(response->body))
            {
                return llm::completion_output(config.format, *raw);
            }
            return std::unexpected(app::ModelError{"模型结构化补全响应格式无效"});
        }

        std::expected<std::vector<std::string>, app::ModelError>
        list_models(std::stop_token stop = {}) const
        {
            const auto base = model_base(config_);
            const auto headers = model_headers(config_);
            if (!base || !headers)
            {
                return std::unexpected(base ? headers.error() : base.error());
            }
            const auto listing = *base + "/models";
            std::string next;
            std::set<std::string> names;
            for (unsigned int page = 0; page < 20; ++page)
            {
                const auto response =
                    llm::model_http(listing + next, *headers, nullptr, config_.timeout,
                                    config_.max_response_bytes, stop);
                if (!response)
                {
                    return std::unexpected(response.error());
                }
                const auto data = meowlive::protocol::json::parse_object(response->body);
                const bool gemini = config_.format == ProviderFormat::GeminiGenerateContent;
                const char* array_key = gemini ? "models" : "data";
                if (!data || !(*data)[array_key].isArray())
                {
                    return std::unexpected(app::ModelError{"模型列表响应格式无效"});
                }
                for (const auto& item : (*data)[array_key])
                {
                    if (!item.isObject())
                    {
                        continue;
                    }
                    const auto& name = item[gemini ? "name" : "id"];
                    if (!name.isString() || name.asString().empty() ||
                        name.asString().size() > 4096)
                    {
                        continue;
                    }
                    auto id = name.asString();
                    if (gemini)
                    {
                        const auto& methods = item["supportedGenerationMethods"];
                        if (methods.isArray() && std::ranges::none_of(methods,
                                                                      [](const auto& method)
                                                                      {
                                                                          return method ==
                                                                                 "generateContent";
                                                                      }))
                        {
                            continue;
                        }
                        if (id.starts_with("models/"))
                        {
                            id.erase(0, 7);
                        }
                    }
                    names.insert(std::move(id));
                }
                std::string marker;
                if (gemini && (*data)["nextPageToken"].isString())
                {
                    marker = (*data)["nextPageToken"].asString();
                }
                if (config_.format == ProviderFormat::AnthropicMessages &&
                    (*data)["has_more"].isBool() && (*data)["has_more"].asBool() &&
                    (*data)["last_id"].isString())
                {
                    marker = (*data)["last_id"].asString();
                }
                if (marker.empty())
                {
                    return std::vector<std::string>(names.begin(), names.end());
                }
                if (marker.size() > 4096)
                {
                    break;
                }
                auto query =
                    std::string(gemini ? "?pageToken=" : "?after_id=") + encode_query(marker);
                if (query == next)
                {
                    break;
                }
                next = std::move(query);
            }
            return std::unexpected(app::ModelError{"模型列表分页无效或超过上限"});
        }

        std::expected<void, app::ModelError> test_connection(std::stop_token stop = {})
        {
            const app::DecisionRequest request{
                .system_prompt = "Connection test. Return exactly one JSON object with reply_to "
                                 "[], text null and topic null."};
            const auto response = this->turn(request, {.final_turn = true}, stop);
            if (!response)
            {
                return std::unexpected(response.error());
            }
            return {};
        }

    private:
        meowlive::adapters::ProviderConfig config_;
    };
}
