module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.web_search;

import meowlive.standard;
import meowlive.adapters.llm.http;
import meowlive.application.llm;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

namespace
{
    namespace app = meowlive::application;
    namespace llm = meowlive::adapters::llm_detail;

    std::string url_encode(std::string_view text)
    {
        constexpr std::string_view hex = "0123456789ABCDEF";
        std::string result;
        result.reserve(text.size() * 3);
        for (const auto value : text)
        {
            const auto byte = static_cast<unsigned char>(value);
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0xf]);
        }
        return result;
    }

    bool secure_or_loopback(std::string_view endpoint)
    {
        if (endpoint.starts_with("https://"))
        {
            return true;
        }
        endpoint.remove_prefix(7); // validate_model_url has already checked the scheme.
        const auto authority = endpoint.substr(0, endpoint.find('/'));
        return authority == "127.0.0.1" || authority.starts_with("127.0.0.1:") ||
               authority == "[::1]" || authority.starts_with("[::1]:");
    }

    std::string clipped_text(const Json::Value& field, std::size_t limit)
    {
        if (!field.isString())
        {
            return {};
        }
        auto value = field.asString();
        if (value.size() > limit)
        {
            while ((static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80)
            {
                --limit;
            }
            value.resize(limit);
        }
        return value;
    }
}

export namespace meowlive::adapters
{
    struct WebSearchConfig
    {
        std::string provider;
        std::string base_url;
        std::string api_key;
        std::chrono::seconds timeout{10};
    };

    std::expected<std::string, meowlive::application::ModelError>
    search_web(const meowlive::adapters::WebSearchConfig& config, std::string_view query,
               std::stop_token stop = {})
    {
        const bool brave = config.provider == "brave";
        const app::ModelError invalid{"网页搜索配置无效"};
        if ((!brave && config.provider != "searxng") || config.timeout.count() < 1 ||
            config.timeout.count() > 60 ||
            (brave && (config.api_key.empty() || config.api_key.size() > 4096 ||
                       std::ranges::any_of(config.api_key,
                                           [](unsigned char ch)
                                           {
                                               return ch <= 32 || ch >= 127;
                                           }))))
        {
            return std::unexpected(invalid);
        }
        if (query.empty() || query.size() > 600 || !meowlive::protocol::is_utf8(query) ||
            std::ranges::any_of(query,
                                [](unsigned char ch)
                                {
                                    return ch < 32 || ch == 127;
                                }))
        {
            return std::unexpected(app::ModelError{"搜索关键词无效"});
        }

        const auto endpoint = llm::validate_model_url(
            brave && config.base_url.empty() ? "https://api.search.brave.com/res/v1/web/search"
                                             : config.base_url);
        if (!endpoint || !secure_or_loopback(*endpoint))
        {
            return std::unexpected(invalid);
        }
        auto url = *endpoint + (brave ? "?count=5&text_decorations=false&q=" : "?format=json&q=") +
                   url_encode(query);
        const std::array headers{std::string("Accept: application/json"),
                                 "X-Subscription-Token: " + config.api_key};
        const auto response = llm::model_http(url, std::span(headers).first(brave ? 2 : 1), nullptr,
                                              config.timeout, 512 * 1024, stop);
        if (!response)
        {
            return std::unexpected(response.error());
        }
        const auto body = meowlive::protocol::json::parse_object(response->body);
        const app::ModelError malformed{"网页搜索结果格式无效"};
        if (!body || (brave && !(*body)["web"].isNull() && !(*body)["web"].isObject()))
        {
            return std::unexpected(malformed);
        }
        Json::Value output(Json::objectValue);
        auto& results = output["results"] = Json::Value(Json::arrayValue);
        const auto& items = brave ? (*body)["web"]["results"] : (*body)["results"];
        if (!items.isArray() && (!brave || !items.isNull()))
        {
            return std::unexpected(malformed);
        }
        for (const auto& item : items)
        {
            if (!item.isObject())
            {
                continue;
            }
            auto link = clipped_text(item["url"], 2048);
            if (!link.starts_with("https://") && !link.starts_with("http://"))
            {
                continue;
            }
            Json::Value row(Json::objectValue);
            row["title"] = clipped_text(item["title"], 512);
            row["url"] = std::move(link);
            row["snippet"] = clipped_text(item[brave ? "description" : "content"], 1024);
            results.append(std::move(row));
            if (results.size() == 5)
            {
                break;
            }
        }
        return meowlive::protocol::json::serialize(output);
    }
}
