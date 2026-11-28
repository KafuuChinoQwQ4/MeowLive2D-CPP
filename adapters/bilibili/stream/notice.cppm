module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.notice;

import meowlive.standard;
import meowlive.domain.live_event;
import meowlive.protocol.json;

namespace
{
    namespace domain = meowlive::domain;
    namespace json = meowlive::protocol::json;

    bool valid_id(std::string_view text)
    {
        return !text.empty() && text.size() <= 128 &&
               std::ranges::all_of(text,
                                   [](unsigned char byte)
                                   {
                                       return byte >= '!' && byte <= '~';
                                   });
    }

    std::string field_text(const Json::Value& value)
    {
        return value.isString() ? value.asString() : std::string{};
    }

    std::optional<std::uint64_t> unsigned_number(const Json::Value& value)
    {
        if ((value.type() == Json::intValue || value.type() == Json::uintValue) && value.isUInt64())
        {
            return value.asUInt64();
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> unsigned_32(const Json::Value& value)
    {
        return json::is_unsigned_32(value) ? std::optional{value.asUInt()} : std::nullopt;
    }

    std::optional<std::uint64_t> platform_time(const Json::Value& value)
    {
        constexpr std::uint64_t maximum_seconds = 9'007'199'254'740'991ULL / 1000;
        const auto timestamp = unsigned_number(value);
        if (!timestamp || *timestamp == 0 || *timestamp > maximum_seconds)
        {
            return std::nullopt;
        }
        return *timestamp * 1000;
    }

    std::optional<domain::ViewerIdentity> identity(const Json::Value& value, std::uint64_t app_id)
    {
        if (const auto uid = unsigned_number(value["uid"]); uid && *uid)
        {
            return domain::ViewerIdentity{"bilibili:global", domain::ViewerIdentityKind::Uid,
                                          std::to_string(*uid)};
        }
        if (auto open_id = field_text(value["open_id"]); valid_id(open_id))
        {
            return domain::ViewerIdentity{"bilibili:app:" + std::to_string(app_id),
                                          domain::ViewerIdentityKind::OpenId, std::move(open_id)};
        }
        return std::nullopt;
    }

    std::optional<domain::EventKind> event_kind(std::string_view command, const Json::Value& value)
    {
        if (command == "LIVE_OPEN_PLATFORM_DM")
        {
            if (auto text = field_text(value["msg"]); !text.empty())
            {
                return domain::Chat{std::move(text)};
            }
        }
        else if (command == "LIVE_OPEN_PLATFORM_SEND_GIFT")
        {
            auto name = field_text(value["gift_name"]);
            const auto count = unsigned_32(value["gift_num"]);
            if (!name.empty() && count && *count && *count <= 10'000)
            {
                return domain::Gift{std::move(name), *count};
            }
        }
        else if (command == "LIVE_OPEN_PLATFORM_SUPER_CHAT")
        {
            auto text = field_text(value["message"]);
            const auto amount = unsigned_32(value["rmb"]);
            const auto start = platform_time(value["start_time"]);
            const auto end = platform_time(value["end_time"]);
            if (!text.empty() && amount && *amount && *amount <= 1'000'000 && start && end &&
                *end > *start)
            {
                return domain::SuperChat{std::move(text), *amount, *start, *end};
            }
        }
        else if (command == "LIVE_OPEN_PLATFORM_LIVE_ROOM_ENTER")
        {
            return domain::RoomEnter{};
        }
        return std::nullopt;
    }
}

export namespace meowlive::adapters::bilibili
{
    struct Notice
    {
        std::optional<meowlive::domain::LiveEvent> event;
        bool interaction_ended{};
    };

    meowlive::adapters::bilibili::Notice decode_notice(std::string_view body, std::uint64_t room,
                                                       std::uint64_t app_id,
                                                       std::string_view game_id)
    {
        const auto envelope = json::parse_object(body);
        if (!envelope || !(*envelope)["data"].isObject())
        {
            return {};
        }
        const auto command = field_text((*envelope)["cmd"]);
        const auto& data = (*envelope)["data"];
        if (command == "LIVE_OPEN_PLATFORM_INTERACTION_END")
        {
            return {.interaction_ended =
                        !game_id.empty() && field_text(data["game_id"]) == game_id};
        }
        if (unsigned_number(data["room_id"]) != room)
        {
            return {};
        }
        const auto timestamp = platform_time(data["timestamp"]);
        auto viewer = field_text(data["uname"]);
        auto kind = event_kind(command, data);
        if (!timestamp || viewer.empty() || !kind)
        {
            return {};
        }

        auto message = field_text(data["msg_id"]);
        if (command == "LIVE_OPEN_PLATFORM_SUPER_CHAT")
        {
            const auto& sc_id = data["message_id"];
            if (const auto number = unsigned_number(sc_id); number && *number)
            {
                message = std::to_string(*number);
            }
            else
            {
                message = field_text(sc_id);
            }
            if (!valid_id(message))
            {
                return {};
            }
            message.insert(0, "sc:");
        }
        auto id = "bilibili:" + std::to_string(room) + ":" + message;
        if (!valid_id(message) || !valid_id(id))
        {
            return {};
        }

        std::optional<domain::GiftMetadata> metadata;
        if (std::holds_alternative<domain::Gift>(*kind))
        {
            auto gift = domain::GiftMetadata{
                unsigned_number(data["price"]),
                data["paid"].isBool() ? std::optional{data["paid"].asBool()} : std::nullopt,
                unsigned_32(data["fans_medal_level"]), unsigned_32(data["guard_level"])};
            if (gift.price || gift.paid || gift.medal_level || gift.guard_level)
            {
                metadata = gift;
            }
        }
        return {.event = domain::LiveEvent{std::move(id), "bilibili", std::move(viewer),
                                           identity(data, app_id), *timestamp, metadata,
                                           std::move(*kind)}};
    }
}
