module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.stream;

import meowlive.standard;
import meowlive.adapters.llm.provider_config;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

namespace
{
    using meowlive::adapters::ProviderFormat;

    bool append(Json::Value& destination, const Json::Value& delta)
    {
        if (delta.isNull())
        {
            return true;
        }
        if (!delta.isString() || (!destination.isNull() && !destination.isString()))
        {
            return false;
        }
        destination =
            (destination.isNull() ? std::string{} : destination.asString()) + delta.asString();
        return true;
    }

    void merge_usage(Json::Value& destination, const Json::Value& source)
    {
        if (source.isObject())
        {
            for (auto item = source.begin(); item != source.end(); ++item)
            {
                destination[item.name()] = *item;
            }
        }
    }

    class Stream
    {
    public:
        explicit Stream(ProviderFormat format) : format_(format)
        {
            if (format == ProviderFormat::OpenaiChat)
            {
                raw_["choices"][0]["message"]["role"] = "assistant";
            }
            else if (format == ProviderFormat::AnthropicMessages)
            {
                raw_["role"] = "assistant";
                raw_["content"] = Json::Value(Json::arrayValue);
            }
            else if (format == ProviderFormat::GeminiGenerateContent)
            {
                raw_["candidates"][0]["content"]["role"] = "model";
                raw_["candidates"][0]["content"]["parts"] = Json::Value(Json::arrayValue);
            }
        }

        bool consume(std::string_view data)
        {
            if (data == "[DONE]")
            {
                if (format_ != ProviderFormat::OpenaiChat || terminal_ ||
                    !raw_["choices"][0]["finish_reason"].isString())
                {
                    return false;
                }
                terminal_ = true;
                return true;
            }
            const auto event = meowlive::protocol::json::parse_object(data);
            if (!event || event->isMember("error") || (*event)["type"] == "error")
            {
                return false;
            }
            using enum ProviderFormat;
            switch (format_)
            {
            case OpenaiChat:
                return !terminal_ && this->chat(*event);
            case OpenaiResponses:
                return !terminal_ && this->responses(*event);
            case AnthropicMessages:
                return !terminal_ && this->anthropic(*event);
            case GeminiGenerateContent:
                return this->gemini(*event);
            }
            return false;
        }

        std::optional<Json::Value> result()
        {
            if (!terminal_)
            {
                return std::nullopt;
            }
            return std::move(raw_);
        }

    private:
        bool chat(const Json::Value& event)
        {
            merge_usage(raw_["usage"], event["usage"]);
            const auto& choices = event["choices"];
            if (!choices.isArray())
            {
                return false;
            }
            if (choices.empty())
            {
                return true;
            }
            auto& choice = raw_["choices"][0];
            if (choices.size() != 1 || !choices[0].isObject() ||
                (!choices[0]["index"].isNull() && choices[0]["index"] != 0) ||
                !choice["finish_reason"].isNull())
            {
                return false;
            }
            const auto& delta = choices[0]["delta"];
            auto& message = choice["message"];
            if (!delta.isObject() || !delta["refusal"].isNull() ||
                !delta["function_call"].isNull() || !append(message["content"], delta["content"]) ||
                !append(message["reasoning_content"], delta["reasoning_content"]) ||
                !append(message["reasoning"], delta["reasoning"]))
            {
                return false;
            }
            const auto& calls = delta["tool_calls"];
            if (!calls.isNull() && !calls.isArray())
            {
                return false;
            }
            for (const auto& call : calls)
            {
                if (!call.isObject() || !call["index"].isUInt() || call["index"].asUInt() >= 4 ||
                    call["index"].asUInt() > message["tool_calls"].size() ||
                    (!call["type"].isNull() && call["type"] != "function") ||
                    !call["function"].isObject())
                {
                    return false;
                }
                auto& target = message["tool_calls"][call["index"].asUInt()];
                target["type"] = "function";
                if (!append(target["id"], call["id"]) ||
                    !append(target["function"]["name"], call["function"]["name"]) ||
                    !append(target["function"]["arguments"], call["function"]["arguments"]))
                {
                    return false;
                }
            }
            if (!choices[0]["finish_reason"].isNull())
            {
                choice["finish_reason"] = choices[0]["finish_reason"];
            }
            return true;
        }

        bool responses(const Json::Value& event)
        {
            const auto& type = event["type"];
            if (type == "response.completed")
            {
                if (!event["response"].isObject() || event["response"]["status"] != "completed")
                {
                    return false;
                }
                raw_ = event["response"];
                terminal_ = true;
                return true;
            }
            return type.isString() && type != "response.failed" && type != "response.incomplete" &&
                   type != "response.refusal.delta" && type != "response.refusal.done";
        }

        bool anthropic(const Json::Value& event)
        {
            const auto& type = event["type"];
            if (type == "ping")
            {
                return true;
            }
            if (type == "message_start")
            {
                if (started_ || !event["message"].isObject() ||
                    event["message"]["role"] != "assistant")
                {
                    return false;
                }
                started_ = true;
                merge_usage(raw_["usage"], event["message"]["usage"]);
                return true;
            }
            if (!started_)
            {
                return false;
            }
            if (type == "message_delta")
            {
                if (!event["delta"].isObject())
                {
                    return false;
                }
                raw_["stop_reason"] = event["delta"]["stop_reason"];
                merge_usage(raw_["usage"], event["usage"]);
                return true;
            }
            if (type == "message_stop")
            {
                terminal_ = raw_["stop_reason"].isString() && !block_open_;
                return terminal_;
            }
            if (!event["index"].isUInt())
            {
                return false;
            }
            const auto index = event["index"].asUInt();
            auto& content = raw_["content"];
            if (type == "content_block_start")
            {
                if (block_open_ || index != content.size() || index >= 64 ||
                    !event["content_block"].isObject())
                {
                    return false;
                }
                content.append(event["content_block"]);
                block_open_ = true;
                return true;
            }
            if (!block_open_ || index + 1U != content.size())
            {
                return false;
            }
            auto& block = content[index];
            if (type == "content_block_stop")
            {
                if (!arguments_.empty())
                {
                    const auto input = meowlive::protocol::json::parse_object(arguments_);
                    if (!input)
                    {
                        return false;
                    }
                    block["input"] = *input;
                    arguments_.clear();
                }
                block_open_ = false;
                return true;
            }
            const auto& delta = event["delta"];
            if (type != "content_block_delta" || !delta.isObject())
            {
                return false;
            }
            if (delta["type"] == "input_json_delta" && block["type"] == "tool_use" &&
                delta["partial_json"].isString())
            {
                arguments_ += delta["partial_json"].asString();
                return arguments_.size() <= 16 * 1024;
            }
            if (delta["type"] == "text_delta" && block["type"] == "text")
            {
                return append(block["text"], delta["text"]);
            }
            if (delta["type"] == "thinking_delta" && block["type"] == "thinking")
            {
                return append(block["thinking"], delta["thinking"]);
            }
            if (delta["type"] == "signature_delta" && block["type"] == "thinking")
            {
                return append(block["signature"], delta["signature"]);
            }
            return false;
        }

        bool gemini(const Json::Value& event)
        {
            merge_usage(raw_["usageMetadata"], event["usageMetadata"]);
            const auto& candidates = event["candidates"];
            if (candidates.isNull() || (candidates.isArray() && candidates.empty()))
            {
                return true;
            }
            if (terminal_ || !candidates.isArray() || candidates.size() != 1 ||
                !candidates[0].isObject())
            {
                return false;
            }
            const auto& candidate = candidates[0];
            auto& destination = raw_["candidates"][0];
            const auto& content = candidate["content"];
            if (!content.isNull())
            {
                if (!content.isObject() || !content["parts"].isArray())
                {
                    return false;
                }
                for (const auto& part : content["parts"])
                {
                    if (!part.isObject())
                    {
                        return false;
                    }
                    auto& parts = destination["content"]["parts"];
                    if (parts.size() >= 4096)
                    {
                        return false;
                    }
                    parts.append(part);
                }
            }
            if (!candidate["finishReason"].isNull())
            {
                destination["finishReason"] = candidate["finishReason"];
                terminal_ = true;
            }
            return true;
        }

        ProviderFormat format_;
        Json::Value raw_{Json::objectValue};
        bool terminal_{};
        bool started_{};
        bool block_open_{};
        std::string arguments_;
    };
}

export namespace meowlive::adapters::llm_detail
{
    std::optional<Json::Value> parse_sse(meowlive::adapters::ProviderFormat format,
                                         std::string_view bytes)
    {
        Stream stream(format);
        std::string data;
        while (!bytes.empty())
        {
            const auto end = bytes.find('\n');
            if (end == std::string_view::npos)
            {
                if (bytes.find_first_not_of(" \t\r") != std::string_view::npos)
                {
                    return std::nullopt;
                }
                break;
            }
            auto line = bytes.substr(0, end);
            bytes.remove_prefix(end + 1);
            if (line.ends_with('\r'))
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                if (!data.empty())
                {
                    data.pop_back();
                    if (!stream.consume(data))
                    {
                        return std::nullopt;
                    }
                    data.clear();
                }
            }
            else if (line.starts_with("data:"))
            {
                line.remove_prefix(5);
                if (line.starts_with(' '))
                {
                    line.remove_prefix(1);
                }
                data += line;
                data += '\n';
            }
            else if (!meowlive::protocol::is_utf8(line))
            {
                return std::nullopt;
            }
        }
        if (!data.empty())
        {
            return std::nullopt;
        }
        return stream.result();
    }
}
