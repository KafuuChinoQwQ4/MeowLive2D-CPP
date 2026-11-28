module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.agent_json;

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.agent.session;
import meowlive.domain.live_event;
import meowlive.domain.speech;
import meowlive.protocol.json;

namespace
{
    namespace domain = meowlive::domain;
    namespace agent = meowlive::agent;

    bool valid_text(const ::Json::Value& value, unsigned limit)
    {
        return value.isString() &&
               domain::normalize_speech_text(value.asString(), limit).has_value();
    }

    bool has_only(const ::Json::Value& value, std::initializer_list<std::string_view> fields)
    {
        return value.isObject() &&
               std::ranges::all_of(value.getMemberNames(),
                                   [&](const auto& key)
                                   {
                                       return std::ranges::find(fields, key) != fields.end();
                                   });
    }

    std::optional<domain::LiveEvent> decode(const ::Json::Value& value)
    {
        if (!has_only(value,
                      {"id", "source", "viewer", "kind", "viewer_identity", "gift_metadata"}) ||
            !valid_text(value["id"], 128) || !valid_text(value["source"], 64) ||
            !valid_text(value["viewer"], 80))
        {
            return {};
        }
        domain::LiveEvent event;
        event.id = value["id"].asString();
        event.source = value["source"].asString();
        event.viewer = value["viewer"].asString();
        const auto& identity = value["viewer_identity"];
        if (!identity.isNull())
        {
            if (!has_only(identity, {"namespace", "kind", "external_id"}) ||
                !valid_text(identity["namespace"], 64) ||
                !valid_text(identity["external_id"], 128) || !identity["kind"].isString())
            {
                return {};
            }
            const auto kind = identity["kind"].asString();
            if (kind != "open_id" && kind != "uid")
            {
                return {};
            }
            event.viewer_identity =
                domain::ViewerIdentity{identity["namespace"].asString(),
                                       kind == "uid" ? domain::ViewerIdentityKind::Uid
                                                     : domain::ViewerIdentityKind::OpenId,
                                       identity["external_id"].asString()};
        }
        const auto& kind = value["kind"];
        if (!kind.isObject() || !kind["type"].isString())
        {
            return {};
        }
        const auto type = kind["type"].asString();
        const auto positive = [&kind](const char* field)
        {
            return meowlive::protocol::json::is_unsigned_32(kind[field]) &&
                   kind[field].asUInt() > 0;
        };
        if (type == "chat" && has_only(kind, {"type", "text"}) && valid_text(kind["text"], 500))
        {
            event.kind = domain::Chat{kind["text"].asString()};
        }
        else if (type == "gift" && has_only(kind, {"type", "name", "count"}) &&
                 valid_text(kind["name"], 80) && positive("count"))
        {
            event.kind = domain::Gift{kind["name"].asString(), kind["count"].asUInt()};
        }
        else if (type == "super_chat" &&
                 has_only(kind, {"type", "text", "amount_cny", "start_at_ms", "end_at_ms"}) &&
                 valid_text(kind["text"], 500) && positive("amount_cny") &&
                 kind["start_at_ms"].isUInt64() && kind["end_at_ms"].isUInt64() &&
                 kind["end_at_ms"].asUInt64() > kind["start_at_ms"].asUInt64())
        {
            event.kind =
                domain::SuperChat{kind["text"].asString(), kind["amount_cny"].asUInt(),
                                  kind["start_at_ms"].asUInt64(), kind["end_at_ms"].asUInt64()};
        }
        else if (type == "room_enter" && kind.size() == 1)
        {
            event.kind = domain::RoomEnter{};
        }
        else
        {
            return {};
        }

        const auto& metadata = value["gift_metadata"];
        if (!metadata.isNull())
        {
            if (type != "gift" ||
                !has_only(metadata, {"price", "paid", "medal_level", "guard_level"}) ||
                (!metadata["price"].isNull() && !metadata["price"].isUInt64()) ||
                (!metadata["paid"].isNull() && !metadata["paid"].isBool()) ||
                (!metadata["medal_level"].isNull() &&
                 !meowlive::protocol::json::is_unsigned_32(metadata["medal_level"])) ||
                (!metadata["guard_level"].isNull() &&
                 !meowlive::protocol::json::is_unsigned_32(metadata["guard_level"])))
            {
                return {};
            }
            domain::GiftMetadata gift;
            if (metadata["price"].isUInt64())
                gift.price = metadata["price"].asUInt64();
            if (metadata["paid"].isBool())
                gift.paid = metadata["paid"].asBool();
            if (metadata["medal_level"].isUInt())
                gift.medal_level = metadata["medal_level"].asUInt();
            if (metadata["guard_level"].isUInt())
                gift.guard_level = metadata["guard_level"].asUInt();
            event.gift_metadata = gift;
        }
        return event;
    }
}

export namespace meowlive::server::agent_json
{
    ::Json::Value event(const meowlive::domain::LiveEvent& event)
    {
        ::Json::Value json(::Json::objectValue);
        json["id"] = event.id;
        json["source"] = event.source;
        json["viewer"] = event.viewer;
        if (const auto& id = event.viewer_identity)
        {
            json["viewer_identity"]["namespace"] = id->identity_namespace;
            json["viewer_identity"]["external_id"] = id->external_id;
            json["viewer_identity"]["kind"] =
                id->kind == meowlive::domain::ViewerIdentityKind::Uid ? "uid" : "open_id";
        }
        if (const auto& metadata = event.gift_metadata)
        {
            if (metadata->price)
                json["gift_metadata"]["price"] = ::Json::UInt64(*metadata->price);
            if (metadata->paid)
                json["gift_metadata"]["paid"] = *metadata->paid;
            if (metadata->medal_level)
                json["gift_metadata"]["medal_level"] = *metadata->medal_level;
            if (metadata->guard_level)
                json["gift_metadata"]["guard_level"] = *metadata->guard_level;
        }
        auto& kind = json["kind"];
        if (const auto* chat = std::get_if<meowlive::domain::Chat>(&event.kind))
        {
            kind["type"] = "chat";
            kind["text"] = chat->text;
        }
        else if (const auto* gift = std::get_if<meowlive::domain::Gift>(&event.kind))
        {
            kind["type"] = "gift";
            kind["name"] = gift->name;
            kind["count"] = gift->count;
        }
        else if (const auto* sc = std::get_if<meowlive::domain::SuperChat>(&event.kind))
        {
            kind["type"] = "super_chat";
            kind["text"] = sc->text;
            kind["amount_cny"] = sc->amount_cny;
            kind["start_at_ms"] = ::Json::UInt64(sc->start_at_ms);
            kind["end_at_ms"] = ::Json::UInt64(sc->end_at_ms);
        }
        else
        {
            kind["type"] = "room_enter";
        }
        return json;
    }

    std::optional<std::vector<meowlive::domain::LiveEvent>> events(const ::Json::Value& json)
    {
        const auto& values = json["events"];
        if (!json.isObject() || json.size() != 1 || !values.isArray() || values.empty() ||
            values.size() > 100)
        {
            return {};
        }
        std::vector<meowlive::domain::LiveEvent> decoded;
        decoded.reserve(values.size());
        for (const auto& item : values)
        {
            auto value = decode(item);
            if (!value)
            {
                return {};
            }
            decoded.push_back(std::move(*value));
        }
        return decoded;
    }

    ::Json::Value settings(const meowlive::agent::AgentSettings& settings)
    {
        ::Json::Value value(::Json::objectValue);
        value["persona"] = settings.persona;
        value["system_prompt"] = settings.system_prompt;
        value["topic"] = settings.topic;
        value["proactive_enabled"] = settings.proactive_enabled;
        value["cooldown_ms"] = ::Json::UInt64(settings.cooldown_ms);
        auto& interaction = value["interaction"];
        using Mode = meowlive::agent::ChatMode;
        interaction["chat_read_mode"] = settings.interaction.chat_mode == Mode::All ? "all"
                                        : settings.interaction.chat_mode == Mode::Selective
                                            ? "selective"
                                            : "auto";
        interaction["welcome_enabled"] = settings.interaction.welcome_enabled;
        interaction["busy_chat_count"] = settings.interaction.busy_chat_count;
        interaction["busy_enter_count"] = settings.interaction.busy_enter_count;
        interaction["busy_pending_count"] = settings.interaction.busy_pending_count;
        interaction["welcome_cooldown_ms"] =
            ::Json::UInt64(settings.interaction.welcome_cooldown_ms);
        interaction["welcome_viewer_cooldown_ms"] =
            ::Json::UInt64(settings.interaction.welcome_viewer_cooldown_ms);
        return value;
    }

    meowlive::agent::AgentSettings read_settings(const ::Json::Value& value)
    {
        meowlive::agent::AgentSettings settings;
        if (!value.isObject())
        {
            return settings;
        }
        settings.persona = value.get("persona", settings.persona).asString();
        settings.system_prompt = value.get("system_prompt", "").asString();
        settings.topic = value.get("topic", "").asString();
        settings.proactive_enabled = value.get("proactive_enabled", false).asBool();
        settings.cooldown_ms = value.get("cooldown_ms", 30'000).asUInt64();
        if (const auto& interaction = value["interaction"]; interaction.isObject())
        {
            auto& target = settings.interaction;
            const auto mode = interaction.get("chat_read_mode", "auto").asString();
            target.chat_mode = mode == "all"         ? meowlive::agent::ChatMode::All
                               : mode == "selective" ? meowlive::agent::ChatMode::Selective
                                                     : meowlive::agent::ChatMode::Auto;
            target.welcome_enabled = interaction.get("welcome_enabled", true).asBool();
            target.busy_chat_count = interaction.get("busy_chat_count", 6).asUInt();
            target.busy_enter_count = interaction.get("busy_enter_count", 3).asUInt();
            target.busy_pending_count = interaction.get("busy_pending_count", 4).asUInt();
            target.welcome_cooldown_ms = interaction.get("welcome_cooldown_ms", 30'000).asUInt64();
            target.welcome_viewer_cooldown_ms =
                interaction.get("welcome_viewer_cooldown_ms", 600'000).asUInt64();
        }
        return settings;
    }
}
