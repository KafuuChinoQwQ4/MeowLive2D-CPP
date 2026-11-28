module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.output;

import meowlive.standard;
import meowlive.adapters.llm.provider_config;
import meowlive.adapters.llm.request;
import meowlive.application.llm;
import meowlive.protocol.json;

namespace
{
    namespace app = meowlive::application;
    namespace adapters = meowlive::adapters;
    namespace llm = meowlive::adapters::llm_detail;

    struct Output
    {
        std::string text;
        Json::Value assistant;
        Json::Value calls{Json::arrayValue};
        std::string reason;
    };

    bool collect_text(const Json::Value& block, const char* field, std::string& result)
    {
        if (block[field].isNull())
        {
            return true;
        }
        if (!block[field].isString())
        {
            return false;
        }
        result += block[field].asString();
        return true;
    }

    bool finish(const Json::Value& reason, std::string& target,
                std::initializer_list<std::string_view> allowed)
    {
        if (!reason.isString())
        {
            return false;
        }
        target = reason.asString();
        return std::ranges::find(allowed, target) != allowed.end();
    }

    bool add_call(Output& output, const Json::Value& block, const char* id_key,
                  const char* args_key, const Json::Value* function = nullptr, bool gemini = false)
    {
        if (output.calls.size() == 4)
        {
            return false;
        }
        const auto& callable = function ? *function : block;
        const auto& id = block[id_key];
        const auto& name = callable["name"];
        const auto& arguments = callable[args_key];
        if (!name.isString() || name.asString().empty() ||
            ((!gemini || !id.isNull()) && (!id.isString() || id.asString().empty())))
        {
            return false;
        }
        std::string json;
        if (arguments.isString())
        {
            json = arguments.asString();
            if (!meowlive::protocol::json::parse_object(json))
            {
                return false;
            }
        }
        else if (arguments.isObject())
        {
            json = meowlive::protocol::json::serialize(arguments);
        }
        else if (gemini && arguments.isNull())
        {
            json = "{}";
        }
        else
        {
            return false;
        }
        if (json.size() > 16 * 1024)
        {
            return false;
        }
        Json::Value call(Json::objectValue);
        call["provider_id"] = !id.isNull();
        call["id"] =
            id.isString() ? id.asString() : "gemini-call-" + std::to_string(output.calls.size());
        call["name"] = name;
        call["arguments"] = std::move(json);
        output.calls.append(std::move(call));
        return true;
    }

    bool chat(const Json::Value& root, Output& output)
    {
        const auto& choices = root["choices"];
        if (!choices.isArray() || choices.size() != 1 || !choices[0].isObject() ||
            !finish(choices[0]["finish_reason"], output.reason, {"stop", "tool_calls"}))
        {
            return false;
        }
        const auto& assistant = choices[0]["message"];
        if (!assistant.isObject() || assistant["role"] != "assistant" ||
            !assistant["refusal"].isNull() || !collect_text(assistant, "content", output.text))
        {
            return false;
        }
        const auto& calls = assistant["tool_calls"];
        if (!calls.isNull() && !calls.isArray())
        {
            return false;
        }
        for (const auto& call : calls)
        {
            if (!call.isObject() || call["type"] != "function" || !call["function"].isObject() ||
                !add_call(output, call, "id", "arguments", &call["function"]))
            {
                return false;
            }
        }
        output.assistant = assistant;
        return output.calls.empty() == (output.reason == "stop");
    }

    bool responses(const Json::Value& root, Output& output)
    {
        const auto& items = root["output"];
        if (root["status"] != "completed" || !items.isArray())
        {
            return false;
        }
        output.reason = "completed";
        output.assistant = items;
        for (const auto& item : items)
        {
            if (!item.isObject())
            {
                return false;
            }
            const auto& type = item["type"];
            if (type == "function_call")
            {
                if (!add_call(output, item, "call_id", "arguments"))
                {
                    return false;
                }
            }
            else if (type == "message")
            {
                if (item["role"] != "assistant" || !item["content"].isArray())
                {
                    return false;
                }
                for (const auto& part : item["content"])
                {
                    if (!part.isObject() || part["type"] != "output_text" ||
                        !collect_text(part, "text", output.text))
                    {
                        return false;
                    }
                }
            }
            else if (type != "reasoning")
            {
                return false;
            }
        }
        return true;
    }

    bool anthropic(const Json::Value& root, Output& output)
    {
        const auto& content = root["content"];
        if (!content.isArray() || root["role"] != "assistant" ||
            !finish(root["stop_reason"], output.reason, {"end_turn", "stop_sequence", "tool_use"}))
        {
            return false;
        }
        output.assistant["role"] = "assistant";
        output.assistant["content"] = content;
        for (const auto& item : content)
        {
            if (!item.isObject())
            {
                return false;
            }
            const auto& type = item["type"];
            if (type == "tool_use")
            {
                if (!add_call(output, item, "id", "input"))
                {
                    return false;
                }
            }
            else if (type == "text")
            {
                if (!collect_text(item, "text", output.text))
                {
                    return false;
                }
            }
            else if (type != "thinking" && type != "redacted_thinking")
            {
                return false;
            }
        }
        return output.calls.empty() != (output.reason == "tool_use");
    }

    bool gemini(const Json::Value& root, Output& output)
    {
        const auto& candidates = root["candidates"];
        if (!candidates.isArray() || candidates.size() != 1 || !candidates[0].isObject() ||
            !finish(candidates[0]["finishReason"], output.reason, {"STOP"}))
        {
            return false;
        }
        const auto& content = candidates[0]["content"];
        if (!content.isObject() || !content["parts"].isArray())
        {
            return false;
        }
        output.assistant = content;
        output.assistant["role"] = "model";
        for (const auto& part : content["parts"])
        {
            if (!part.isObject())
            {
                return false;
            }
            if (part["functionCall"].isObject())
            {
                if (!add_call(output, part["functionCall"], "id", "args", nullptr, true))
                {
                    return false;
                }
            }
            else if (part["thought"] != true && !collect_text(part, "text", output.text))
            {
                return false;
            }
        }
        return true;
    }

    std::uint64_t count(const Json::Value& value)
    {
        return value.isUInt64() ? value.asUInt64() : 0;
    }

    std::optional<std::uint64_t> optional_count(const Json::Value& object, const char* field)
    {
        if (object.isObject() && object[field].isUInt64())
        {
            return object[field].asUInt64();
        }
        return std::nullopt;
    }

    std::uint64_t add_count(std::uint64_t left, std::uint64_t right)
    {
        return left + std::min(right, std::numeric_limits<std::uint64_t>::max() - left);
    }

    app::TokenUsage usage(adapters::ProviderFormat format, const Json::Value& root)
    {
        using enum adapters::ProviderFormat;
        const auto& value = root[format == GeminiGenerateContent ? "usageMetadata" : "usage"];
        if (!value.isObject())
        {
            return {};
        }
        app::TokenUsage usage;
        switch (format)
        {
        case OpenaiChat:
            usage.input = count(value["prompt_tokens"]);
            usage.output = count(value["completion_tokens"]);
            usage.cache_read = optional_count(value["prompt_tokens_details"], "cached_tokens");
            if (!usage.cache_read)
            {
                usage.cache_read = optional_count(value, "prompt_cache_hit_tokens");
            }
            usage.cache_write =
                optional_count(value["prompt_tokens_details"], "cache_write_tokens");
            usage.reasoning =
                optional_count(value["completion_tokens_details"], "reasoning_tokens");
            break;
        case OpenaiResponses:
            usage.input = count(value["input_tokens"]);
            usage.output = count(value["output_tokens"]);
            usage.cache_read = optional_count(value["input_tokens_details"], "cached_tokens");
            usage.cache_write = optional_count(value["input_tokens_details"], "cache_write_tokens");
            usage.reasoning = optional_count(value["output_tokens_details"], "reasoning_tokens");
            break;
        case AnthropicMessages:
            usage.input = count(value["input_tokens"]);
            usage.output = count(value["output_tokens"]);
            usage.cache_read = optional_count(value, "cache_read_input_tokens");
            usage.cache_write = optional_count(value, "cache_creation_input_tokens");
            usage.reasoning = optional_count(value["output_tokens_details"], "thinking_tokens");
            usage.input = add_count(usage.input, usage.cache_read.value_or(0));
            usage.input = add_count(usage.input, usage.cache_write.value_or(0));
            break;
        case GeminiGenerateContent:
            usage.input = count(value["promptTokenCount"]);
            usage.output = count(value["candidatesTokenCount"]);
            usage.total = count(value["totalTokenCount"]);
            usage.cache_read = optional_count(value, "cachedContentTokenCount");
            usage.reasoning = optional_count(value, "thoughtsTokenCount");
            usage.output = add_count(usage.output, usage.reasoning.value_or(0));
            break;
        }
        if (!usage.total)
        {
            usage.total = count(value["total_tokens"]);
        }
        if (!usage.total)
        {
            usage.total = add_count(usage.input, usage.output);
        }
        return usage;
    }

    std::string_view object_text(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \r\n\t");
        if (first == std::string_view::npos)
        {
            return {};
        }
        text = text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
        if (text.starts_with("```json") && text.ends_with("```") && text.size() > 10)
        {
            text = text.substr(7, text.size() - 10);
        }
        return text;
    }

    std::optional<app::AgentDecision> decision(std::string_view text)
    {
        const auto object = meowlive::protocol::json::parse_object(object_text(text));
        if (!object || !(*object)["reply_to"].isArray() || !object->isMember("text") ||
            !object->isMember("topic"))
        {
            return std::nullopt;
        }
        app::AgentDecision result;
        for (const auto& id : (*object)["reply_to"])
        {
            if (!id.isString())
            {
                return std::nullopt;
            }
            result.reply_to.push_back(id.asString());
        }
        const auto& output = (*object)["text"];
        const auto& topic = (*object)["topic"];
        if ((!output.isNull() && !output.isString()) || (!topic.isNull() && !topic.isString()))
        {
            return std::nullopt;
        }
        if (output.isString())
        {
            result.text = output.asString();
        }
        if (topic.isString())
        {
            result.topic = topic.asString();
        }
        return result;
    }
    bool read_output(adapters::ProviderFormat format, const Json::Value& root, Output& output)
    {
        using enum adapters::ProviderFormat;
        switch (format)
        {
        case OpenaiChat:
            return chat(root, output);
        case OpenaiResponses:
            return responses(root, output);
        case AnthropicMessages:
            return anthropic(root, output);
        case GeminiGenerateContent:
            return gemini(root, output);
        }
        return false;
    }
}

export namespace meowlive::adapters::llm_detail
{
    std::expected<std::string, app::ModelError> completion_output(adapters::ProviderFormat format,
                                                                  const Json::Value& root)
    {
        Output output;
        if (!read_output(format, root, output) || !output.calls.empty())
        {
            return std::unexpected(app::ModelError{"模型结构化补全格式无效"});
        }
        const auto text = object_text(output.text);
        if (!meowlive::protocol::json::parse_object(text))
        {
            return std::unexpected(app::ModelError{"模型结构化补全格式无效"});
        }
        return std::string(text);
    }

    std::expected<app::ModelTurn, app::ModelError>
    model_output(const adapters::ProviderConfig& config, const Json::Value& root,
                 const app::ModelOptions& options)
    {
        Output output;
        const app::ModelError invalid{"模型决策或工具调用格式无效"};
        if (!read_output(config.format, root, output) ||
            (options.final_turn && !output.calls.empty()))
        {
            return std::unexpected(invalid);
        }
        app::ModelTurn turn;
        turn.usage = usage(config.format, root);
        turn.finish_reason = std::move(output.reason);
        if (output.calls.empty())
        {
            turn.decision = decision(output.text);
            if (!turn.decision)
            {
                return std::unexpected(invalid);
            }
            return turn;
        }

        std::set<std::string> ids;
        for (const auto& call : output.calls)
        {
            auto id = call["id"].asString();
            auto name = call["name"].asString();
            if (!ids.insert(id).second || std::ranges::none_of(options.tools,
                                                               [&](const auto& tool)
                                                               {
                                                                   return tool.name == name;
                                                               }))
            {
                return std::unexpected(invalid);
            }
            turn.tool_calls.push_back(
                {std::move(id), std::move(name), call["arguments"].asString()});
        }
        Json::Value continuation(Json::objectValue);
        continuation["history"] = llm::continuation_history(config.format, options);
        continuation["assistant"] = std::move(output.assistant);
        continuation["calls"] = std::move(output.calls);
        turn.continuation = meowlive::protocol::json::serialize(continuation);
        if (turn.continuation->size() > 1024 * 1024)
        {
            return std::unexpected(invalid);
        }
        return turn;
    }
}
