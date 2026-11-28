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
    Json::Value text_part(std::string_view text)
    {
        Json::Value value(Json::objectValue);
        value["text"] = std::string(text);
        return value;
    }

    Json::Value message(const char* role, const Json::Value& content, const char* field)
    {
        Json::Value value(Json::objectValue);
        value["role"] = role;
        value[field] = content;
        return value;
    }

    std::optional<Json::Value> tool_definitions(adapters::ProviderFormat format,
                                                const app::ModelOptions& options)
    {
        using enum adapters::ProviderFormat;
        if (options.tools.size() > 16)
        {
            return std::nullopt;
        }
        Json::Value tools(Json::arrayValue);
        std::set<std::string> names;
        for (const auto& tool : options.tools)
        {
            if (tool.name.empty() || tool.name.size() > 64 || tool.description.size() > 2048 ||
                tool.parameters_json.size() > 16 * 1024 || !names.insert(tool.name).second ||
                !std::ranges::all_of(tool.name,
                                     [](char ch)
                                     {
                                         return (ch >= 'a' && ch <= 'z') ||
                                                (ch >= 'A' && ch <= 'Z') ||
                                                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
                                     }))
            {
                return std::nullopt;
            }

            const auto schema = meowlive::protocol::json::parse_object(tool.parameters_json);
            if (!schema || (*schema)["type"] != "object")
            {
                return std::nullopt;
            }
            Json::Value item(Json::objectValue);
            item["name"] = tool.name;
            item["description"] = tool.description;
            if (format == AnthropicMessages)
            {
                item["input_schema"] = *schema;
            }
            else if (format == GeminiGenerateContent)
            {
                item["parametersJsonSchema"] = *schema;
            }
            else
            {
                item["parameters"] = *schema;
                if (format == OpenaiChat)
                {
                    auto inner = std::move(item);
                    item = Json::Value(Json::objectValue);
                    item["function"] = std::move(inner);
                }
                else
                {
                    item["strict"] = false;
                }
                item["type"] = "function";
            }
            tools.append(std::move(item));
        }
        return tools;
    }

    bool append_exchange(Json::Value& messages, adapters::ProviderFormat format,
                         const app::ModelOptions& options,
                         const std::optional<Json::Value>& continuation)
    {
        using enum adapters::ProviderFormat;
        if (!continuation)
        {
            return options.results.empty();
        }
        const auto& history = (*continuation)["history"];
        const auto& assistant = (*continuation)["assistant"];
        const auto& calls = (*continuation)["calls"];
        if (!history.isArray() || history.size() > 32 || !calls.isArray() ||
            calls.size() != options.results.size() || options.results.size() > 4 ||
            (format == OpenaiResponses ? !assistant.isArray() : !assistant.isObject()))
        {
            return false;
        }
        for (const auto& item : history)
        {
            messages.append(item);
        }
        if (format == OpenaiResponses)
        {
            for (const auto& item : assistant)
            {
                messages.append(item);
            }
        }
        else
        {
            messages.append(assistant);
        }

        Json::Value outputs(Json::arrayValue);
        for (const auto& call : calls)
        {
            if (!call.isObject() || !call["id"].isString() || !call["name"].isString())
            {
                return false;
            }
            const auto id = call["id"].asString();
            const auto name = call["name"].asString();
            const auto result =
                std::ranges::find_if(options.results,
                                     [&](const auto& entry)
                                     {
                                         return entry.id == id && entry.name == name;
                                     });
            if (result == options.results.end() || result->output_json.size() > 32 * 1024)
            {
                return false;
            }
            Json::Value value(Json::objectValue);
            switch (format)
            {
            case OpenaiChat:
                value = message("tool", result->output_json);
                value["tool_call_id"] = id;
                messages.append(std::move(value));
                break;
            case OpenaiResponses:
                value["type"] = "function_call_output";
                value["call_id"] = id;
                value["output"] = result->output_json;
                messages.append(std::move(value));
                break;
            case AnthropicMessages:
                value["type"] = "tool_result";
                value["tool_use_id"] = id;
                value["content"] = result->output_json;
                value["is_error"] = result->is_error;
                outputs.append(std::move(value));
                break;
            case GeminiGenerateContent:
            {
                auto& response = value["functionResponse"];
                response["name"] = name;
                if (call["provider_id"] == true)
                {
                    response["id"] = id;
                }
                const auto structured = meowlive::protocol::json::parse_object(result->output_json);
                response["response"][result->is_error ? "error" : "result"] =
                    structured ? *structured : Json::Value(result->output_json);
                outputs.append(std::move(value));
                break;
            }
            }
        }
        if (!outputs.empty())
        {
            messages.append(
                message("user", outputs, format == GeminiGenerateContent ? "parts" : "content"));
        }
        return true;
    }
}
