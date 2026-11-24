module;

#include <json/json.h>

#if defined(_WIN32)
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#endif

export module meowlive.protocol.control;

#if !defined(_WIN32)
import std;
#endif
export import meowlive.protocol.audio;
export import meowlive.protocol.execution;
export import meowlive.protocol.resources;
export import meowlive.protocol.version;
import meowlive.protocol.json;

export namespace meowlive::protocol
{
    inline constexpr std::string_view health_path{"/api/health"};
    inline constexpr std::string_view control_path{"/ws/control"};
    inline constexpr std::string_view audio_path{"/ws/audio"};

    struct ServerHealth
    {
        std::string service;
        std::uint16_t protocol_version{};
        bool bridge_connected{};
    };

    struct ClientHello
    {
        std::uint16_t protocol_version{};
    };

    struct ServerHello
    {
        std::uint16_t protocol_version{};
        std::string session_id;
        std::string bridge_id;
        std::uint32_t generation{};
    };

    struct ServerSpeak
    {
        std::string utterance_id;
        std::uint32_t generation{};
        meowlive::protocol::AudioFormat format;
    };

    struct ServerStop
    {
        std::uint32_t generation{};
    };

    struct ResourceCommand
    {
        std::string request_id;
        meowlive::protocol::DesktopResourceOperation operation;
    };

    struct ResourceResultMessage
    {
        std::string request_id;
        meowlive::protocol::DesktopResourceResult result;
    };

    using ServerCommand =
        std::variant<meowlive::protocol::ServerHello, meowlive::protocol::ServerSpeak,
                     meowlive::protocol::ServerStop, meowlive::protocol::ResourceCommand>;
    using ClientMessage =
        std::variant<meowlive::protocol::ClientHello, meowlive::protocol::ExecutionReceipt,
                     meowlive::protocol::ResourceResultMessage>;
}

namespace
{
    constexpr std::array execution_statuses{
        std::pair{meowlive::protocol::ExecutionStatus::Started, "started"},
        std::pair{meowlive::protocol::ExecutionStatus::Completed, "completed"},
        std::pair{meowlive::protocol::ExecutionStatus::Cancelled, "cancelled"},
        std::pair{meowlive::protocol::ExecutionStatus::Failed, "failed"},
    };

    bool is_unsigned_16(const Json::Value& value)
    {
        return meowlive::protocol::json::is_unsigned_32(value) &&
               value.asUInt() <= std::numeric_limits<std::uint16_t>::max();
    }

    std::expected<meowlive::protocol::ExecutionReceipt, std::string>
    decode_receipt(const Json::Value& receipt)
    {
        if (!receipt.isObject() || !receipt["utterance_id"].isString() ||
            !meowlive::protocol::json::is_unsigned_32(receipt["generation"]) ||
            !receipt["status"].isString() ||
            (!receipt["error"].isString() && !receipt["error"].isNull()))
        {
            return std::unexpected("invalid execution receipt fields");
        }
        const auto status = receipt["status"].asString();
        for (const auto& [candidate, tag] : execution_statuses)
        {
            if (status == tag)
            {
                meowlive::protocol::ExecutionReceipt result{
                    .utterance_id = receipt["utterance_id"].asString(),
                    .generation = receipt["generation"].asUInt(),
                    .status = candidate,
                    .error = {},
                };
                if (receipt["error"].isString())
                {
                    result.error = receipt["error"].asString();
                }
                return result;
            }
        }
        return std::unexpected("unknown execution status");
    }

    Json::Value hello_message(const std::uint16_t version)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "hello";
        result["protocol_version"] = version;
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ClientHello& value)
    {
        return hello_message(value.protocol_version);
    }

    Json::Value to_json(const meowlive::protocol::ServerHello& value)
    {
        auto result = hello_message(value.protocol_version);
        result["session_id"] = value.session_id;
        result["bridge_id"] = value.bridge_id;
        result["generation"] = value.generation;
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ServerSpeak& value)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "speak";
        result["utterance_id"] = value.utterance_id;
        result["generation"] = value.generation;
        result["format"]["sample_rate"] = value.format.sample_rate;
        result["format"]["channels"] = value.format.channels;
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ServerStop& value)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "stop";
        result["generation"] = value.generation;
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ExecutionReceipt& value)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "receipt";
        auto& receipt = result["receipt"];
        receipt["utterance_id"] = value.utterance_id;
        receipt["generation"] = value.generation;
        receipt["error"] = value.error ? Json::Value(*value.error) : Json::Value();
        for (const auto& [status, tag] : execution_statuses)
        {
            if (value.status == status)
            {
                receipt["status"] = tag;
                break;
            }
        }
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ResourceCommand& value)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "resource";
        result["request_id"] = value.request_id;
        result["operation"] = value.operation.json();
        return result;
    }

    Json::Value to_json(const meowlive::protocol::ResourceResultMessage& value)
    {
        Json::Value result(Json::objectValue);
        result["type"] = "resource_result";
        result["request_id"] = value.request_id;
        result["result"] = value.result.json();
        return result;
    }
}

export namespace meowlive::protocol
{
    std::string encode_client_message(const meowlive::protocol::ClientMessage& message)
    {
        return meowlive::protocol::json::serialize(std::visit(
            [](const auto& value)
            {
                return to_json(value);
            },
            message));
    }

    std::string encode_server_command(const meowlive::protocol::ServerCommand& command)
    {
        return meowlive::protocol::json::serialize(std::visit(
            [](const auto& value)
            {
                return to_json(value);
            },
            command));
    }

    std::expected<meowlive::protocol::ClientMessage, std::string>
    decode_client_message(const std::string_view text)
    {
        const auto value = meowlive::protocol::json::parse_object(text);
        if (!value || !(*value)["type"].isString())
        {
            return std::unexpected("invalid client message");
        }
        const auto& message = *value;
        const auto tag = message["type"].asString();
        if (tag == "hello" && is_unsigned_16(message["protocol_version"]))
        {
            return meowlive::protocol::ClientHello{
                static_cast<std::uint16_t>(message["protocol_version"].asUInt())};
        }
        if (tag == "receipt")
        {
            const auto receipt = decode_receipt(message["receipt"]);
            if (!receipt)
            {
                return std::unexpected(receipt.error());
            }
            return *receipt;
        }
        if (tag == "resource_result" && message["request_id"].isString())
        {
            auto result = meowlive::protocol::DesktopResourceResult::decode(message["result"]);
            if (!result)
            {
                return std::unexpected(result.error());
            }
            return meowlive::protocol::ResourceResultMessage{message["request_id"].asString(),
                                                             std::move(*result)};
        }
        return std::unexpected("unknown or invalid client message");
    }

    std::expected<meowlive::protocol::ServerCommand, std::string>
    decode_server_command(const std::string_view text)
    {
        const auto value = meowlive::protocol::json::parse_object(text);
        if (!value || !(*value)["type"].isString())
        {
            return std::unexpected("invalid server command");
        }
        const auto& command = *value;
        const auto tag = command["type"].asString();
        if (tag == "resource" && command["request_id"].isString())
        {
            auto operation =
                meowlive::protocol::DesktopResourceOperation::decode(command["operation"]);
            if (!operation)
            {
                return std::unexpected(operation.error());
            }
            return meowlive::protocol::ResourceCommand{command["request_id"].asString(),
                                                       std::move(*operation)};
        }
        if (!meowlive::protocol::json::is_unsigned_32(command["generation"]))
        {
            return std::unexpected("invalid playback generation");
        }
        const auto generation = command["generation"].asUInt();
        if (tag == "hello" && is_unsigned_16(command["protocol_version"]) &&
            command["session_id"].isString() && command["bridge_id"].isString())
        {
            return meowlive::protocol::ServerHello{
                static_cast<std::uint16_t>(command["protocol_version"].asUInt()),
                command["session_id"].asString(), command["bridge_id"].asString(), generation};
        }
        if (tag == "stop")
        {
            return meowlive::protocol::ServerStop{generation};
        }
        if (tag == "speak" && command["utterance_id"].isString())
        {
            const auto& format = command["format"];
            if (format.isObject() &&
                meowlive::protocol::json::is_unsigned_32(format["sample_rate"]) &&
                is_unsigned_16(format["channels"]))
            {
                return meowlive::protocol::ServerSpeak{
                    command["utterance_id"].asString(),
                    generation,
                    {format["sample_rate"].asUInt(),
                     static_cast<std::uint16_t>(format["channels"].asUInt())}};
            }
        }
        return std::unexpected("unknown or invalid server command");
    }
}
