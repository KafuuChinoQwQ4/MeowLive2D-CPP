module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.memory_embedding;

import meowlive.standard;
import meowlive.adapters.llm.http;
import meowlive.application.llm;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

namespace
{
    constexpr std::size_t response_limit = 64 * 1024;
}

export namespace meowlive::adapters
{
    struct EmbeddingConfig
    {
        std::string endpoint;
        std::string model;
        std::string api_key;
        unsigned dimensions{};
    };

    std::expected<std::string, meowlive::application::ModelError>
    embed_memory(const meowlive::adapters::EmbeddingConfig& config, std::string_view text,
                 std::chrono::seconds timeout, std::stop_token stop)
    {
        if (config.model.empty() || config.model.size() > 256 ||
            !meowlive::protocol::is_utf8(config.model) || config.dimensions == 0 ||
            config.dimensions > 4096 || timeout.count() <= 0 || timeout.count() > 60 ||
            config.api_key.size() > 4096 ||
            std::ranges::any_of(config.api_key,
                                [](unsigned char ch)
                                {
                                    return ch <= 32 || ch >= 127;
                                }) ||
            text.size() > 8192 || !meowlive::protocol::is_utf8(text) ||
            meowlive::protocol::trim_utf8_whitespace(text).empty())
        {
            return std::unexpected(meowlive::application::ModelError{"记忆嵌入配置或输入无效"});
        }
        auto url = meowlive::adapters::llm_detail::validate_model_url(config.endpoint);
        if (!url)
        {
            return std::unexpected(url.error());
        }
        const auto authority = url->find("://") + 3;
        if (url->find('/', authority) == std::string::npos || !url->ends_with("/embeddings"))
        {
            *url += "/embeddings";
        }

        ::Json::Value body(::Json::objectValue);
        body["model"] = config.model;
        body["input"] = ::Json::Value(::Json::arrayValue);
        body["input"].append(std::string(text));
        body["dimensions"] = config.dimensions;
        body["encoding_format"] = "float";
        const auto request = meowlive::protocol::json::serialize(body);
        std::array headers{std::string("Content-Type: application/json"),
                           std::string("Accept: application/json"),
                           "Authorization: Bearer " + config.api_key};
        const auto response = meowlive::adapters::llm_detail::model_http(
            *url, std::span<const std::string>(headers).first(config.api_key.empty() ? 2 : 3),
            &request, timeout, response_limit, stop);
        if (!response)
        {
            return std::unexpected(response.error());
        }

        const auto invalid =
            std::unexpected(meowlive::application::ModelError{"记忆嵌入模型响应无效"});
        const auto result = meowlive::protocol::json::parse_object(response->body);
        if (!result || (*result)["model"] != config.model || !(*result)["data"].isArray() ||
            (*result)["data"].size() != 1)
        {
            return invalid;
        }
        const auto& row = (*result)["data"][0];
        if (!row.isObject() || !meowlive::protocol::json::is_unsigned_32(row["index"]) ||
            row["index"].asUInt() != 0 || !row["embedding"].isArray() ||
            row["embedding"].size() != config.dimensions)
        {
            return invalid;
        }
        ::Json::Value vector(::Json::arrayValue);
        for (const auto& coordinate : row["embedding"])
        {
            if (!coordinate.isNumeric())
            {
                return invalid;
            }
            const auto number = coordinate.asDouble();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
            {
                return invalid;
            }
            vector.append(static_cast<double>(static_cast<float>(number)));
        }
        return meowlive::protocol::json::serialize(vector);
    }
}
