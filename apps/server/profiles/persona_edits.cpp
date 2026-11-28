module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.profile_edits;

import meowlive.standard;

import meowlive.protocol.utf8;

namespace
{
    bool valid_agent(const ::Json::Value& agent)
    {
        if (!meowlive::server::exact_fields(agent,
                                            {"persona", "system_prompt", "topic",
                                             "proactive_enabled", "cooldown_ms", "interaction"}) ||
            !meowlive::server::profile_detail::text(agent["persona"], 2000, false, true) ||
            !meowlive::server::profile_detail::text(agent["system_prompt"], 4000, true, true) ||
            !meowlive::server::profile_detail::text(agent["topic"], 200, true, true) ||
            !agent["proactive_enabled"].isBool() ||
            !meowlive::server::profile_detail::between(agent["cooldown_ms"], 1000, 3'600'000))
        {
            return false;
        }
        const auto& interaction = agent["interaction"];
        return meowlive::server::exact_fields(
                   interaction,
                   {"chat_read_mode", "welcome_enabled", "busy_chat_count", "busy_enter_count",
                    "busy_pending_count", "welcome_cooldown_ms", "welcome_viewer_cooldown_ms"}) &&
               meowlive::server::profile_detail::one_of(interaction["chat_read_mode"],
                                                        {"auto", "all", "selective"}) &&
               interaction["welcome_enabled"].isBool() &&
               meowlive::server::profile_detail::between(interaction["busy_chat_count"], 1, 1000) &&
               meowlive::server::profile_detail::between(interaction["busy_enter_count"], 1,
                                                         1000) &&
               meowlive::server::profile_detail::between(interaction["busy_pending_count"], 1,
                                                         512) &&
               meowlive::server::profile_detail::between(interaction["welcome_cooldown_ms"], 1000,
                                                         3'600'000) &&
               meowlive::server::profile_detail::between(interaction["welcome_viewer_cooldown_ms"],
                                                         1000, 86'400'000);
    }
}

namespace meowlive::server
{
    bool valid_agent_profile_settings(const ::Json::Value& settings)
    {
        return valid_agent(settings);
    }

    std::expected<bool, meowlive::server::ProfileError>
    edit_persona(::Json::Value& document, meowlive::server::ProfileEdit edit,
                 const ::Json::Value& input, const ::Json::Value& defaults)
    {
        using Error = meowlive::server::ProfileError;
        using Edit = meowlive::server::ProfileEdit;
        const auto old_selection = document["selected_profile_id"];
        const auto id = edit == Edit::Save ? old_selection : input["id"];
        auto* selected = meowlive::server::find_profile(document, id);
        const bool create = edit == Edit::Create || (edit == Edit::Save && !selected);
        const bool apply = edit != Edit::Rename && (edit != Edit::Delete || id == old_selection);

        if (edit == Edit::Update && (!id.isString() || id != old_selection))
        {
            return std::unexpected(Error::Stale);
        }
        if (!selected && !create && edit != Edit::Save)
        {
            return std::unexpected(Error::Invalid);
        }

        auto& settings = document["settings"];
        if (edit == Edit::Save)
        {
            if (!input["persona"].isString() || !input["topic"].isString() ||
                !input["proactive_enabled"].isBool() || !input.isMember("cooldown_ms") ||
                (input.isMember("interaction") && !input["interaction"].isObject()))
            {
                return std::unexpected(Error::Invalid);
            }
            settings = meowlive::server::with_profile_defaults(defaults, input);
            if (selected)
            {
                settings["persona"] = (*selected)["persona"];
            }
        }
        if (create)
        {
            ::Json::Value profile(::Json::objectValue);
            if (document["profiles"].size() >= 32 ||
                !meowlive::server::profile_detail::set_name(
                    document, profile, edit == Edit::Save ? ::Json::Value("") : input["name"],
                    meowlive::server::profile_detail::NameRule::Create))
            {
                return std::unexpected(Error::Invalid);
            }
            profile["id"] = drogon::utils::getUuid();
            profile["persona"] = edit == Edit::Save ? settings["persona"] : input["persona"];
            document["selected_profile_id"] = profile["id"];
            document["profiles"].append(std::move(profile));
        }
        else if (edit == Edit::Delete)
        {
            meowlive::server::profile_detail::delete_profile(document, id);
        }
        else if (edit == Edit::Select)
        {
            document["selected_profile_id"] = id;
        }
        else if (edit == Edit::Rename || edit == Edit::Update)
        {
            if (!meowlive::server::profile_detail::set_name(
                    document, *selected, input["name"],
                    meowlive::server::profile_detail::NameRule::Existing))
            {
                return std::unexpected(Error::Invalid);
            }
            if (edit == Edit::Update)
            {
                (*selected)["persona"] = input["persona"];
            }
        }
        if (const auto* profile =
                meowlive::server::find_profile(document, document["selected_profile_id"]))
        {
            settings["persona"] = (*profile)["persona"];
        }
        if (!valid_agent(settings))
        {
            return std::unexpected(Error::Invalid);
        }
        return apply;
    }
}
