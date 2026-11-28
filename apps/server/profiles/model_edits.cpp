module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.profile_edits;

import meowlive.standard;

import meowlive.protocol.utf8;

namespace
{

    constexpr std::initializer_list<std::string_view> public_model_fields{
        "provider",        "api_format", "base_url",  "model",           "mode",
        "timeout_seconds", "max_tokens", "json_mode", "reasoning_effort"};

    bool valid_base_url(std::string_view value)
    {
        if ((!value.starts_with("http://") && !value.starts_with("https://")) ||
            value.find_first_of(" \t\r\n?#\\") != std::string_view::npos)
        {
            return false;
        }
        value.remove_prefix(value.find("://") + 3);
        const auto authority = value.substr(0, value.find('/'));
        return !authority.empty() && !authority.contains('@');
    }

    bool local_endpoint(std::string_view value)
    {
        value.remove_prefix(value.find("://") + 3);
        const auto authority = value.substr(0, value.find('/'));
        const auto host = authority.starts_with("[::1]") ? authority.substr(0, 5)
                                                         : authority.substr(0, authority.find(':'));
        return host == "127.0.0.1" || host == "[::1]";
    }

    bool valid_model(const ::Json::Value& config, bool required, bool listing = false)
    {
        if (!config["base_url"].isString() || !config["model"].isString())
        {
            return false;
        }
        const auto base = config["base_url"].asString();
        const auto model = config["model"].asString();
        const auto& key = config["api_key"];
        const auto key_text = key.isString() ? key.asString() : std::string{};
        if (!meowlive::server::profile_detail::text(config["provider"], 64, false) ||
            config["provider"].asString().size() > 64 ||
            !meowlive::server::profile_detail::one_of(
                config["api_format"], {"openai_chat", "openai_responses", "anthropic_messages",
                                       "gemini_generate_content"}) ||
            !meowlive::server::profile_detail::one_of(config["mode"], {"cloud", "local"}) ||
            !meowlive::server::profile_detail::one_of(
                config["reasoning_effort"],
                {"default", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"}) ||
            !meowlive::server::profile_detail::between(config["timeout_seconds"], 1, 120) ||
            !meowlive::server::profile_detail::between(config["max_tokens"], 64, 65'536) ||
            !meowlive::server::profile_detail::between(config["max_response_bytes"], 1024,
                                                       1'048'576) ||
            !meowlive::server::profile_detail::between(config["max_retries"], 0, 1) ||
            !config["json_mode"].isBool() ||
            (!key.isNull() &&
             (!meowlive::server::profile_detail::text(key, 4096, false) || key_text.size() > 4096 ||
              std::ranges::any_of(key_text,
                                  [](unsigned char byte)
                                  {
                                      return byte <= 32 || byte >= 127;
                                  }))) ||
            !config["api_key_env"].isString() || config["api_key_env"].asString().size() > 128 ||
            (!base.empty() &&
             (!valid_base_url(base) || base.size() > 4096 ||
              (!listing && !meowlive::server::profile_detail::text(config["model"], 128, false)) ||
              model.size() > 128)))
        {
            return false;
        }
        const auto env = config["api_key_env"].asString();
        for (std::size_t index = 0; index < env.size(); ++index)
        {
            const char byte = env[index];
            if (byte != '_' && !(byte >= 'A' && byte <= 'Z') && !(byte >= 'a' && byte <= 'z') &&
                !(index && byte >= '0' && byte <= '9'))
            {
                return false;
            }
        }
        if (config["mode"] == "local" &&
            (base.empty() || !local_endpoint(base) || !env.empty() || !key.isNull() ||
             config["max_tokens"].asUInt() > 1024 || config["max_retries"].asUInt() != 0))
        {
            return false;
        }
        return listing ? !base.empty()
                       : base.empty() == model.empty() && (!required || !model.empty());
    }

    std::string model_destination(const ::Json::Value& config)
    {
        auto value = config["base_url"].asString();
        const auto format = config["api_format"].asString();
        const auto remove_slashes = [&value]
        {
            while (value.ends_with('/'))
            {
                value.pop_back();
            }
        };
        remove_slashes();
        for (const auto [kind, suffix] :
             {std::pair<std::string_view, std::string_view>{"openai_chat", "/chat/completions"},
              {"openai_responses", "/responses"},
              {"anthropic_messages", "/messages"}})
        {
            if (format == kind && value.ends_with(suffix))
            {
                value.resize(value.size() - suffix.size());
                remove_slashes();
                break;
            }
        }
        return value;
    }

    bool same_destination(const ::Json::Value& before, const ::Json::Value& after)
    {
        return before["provider"] == after["provider"] &&
               before["api_format"] == after["api_format"] &&
               model_destination(before) == model_destination(after);
    }

    bool has_model_key(const ::Json::Value& config)
    {
        if (config["api_key"].isString())
        {
            return true;
        }
        const auto env = config["api_key_env"].asString();
        const auto* value = env.empty() ? nullptr : std::getenv(env.c_str());
        return value && !meowlive::server::profile_detail::trim(value).empty();
    }

    ::Json::Value public_model(const ::Json::Value& config)
    {
        ::Json::Value public_fields(::Json::objectValue);
        for (const auto field : public_model_fields)
        {
            const std::string name(field);
            public_fields[name] = config[name];
        }
        return public_fields;
    }
}

namespace meowlive::server
{
    bool valid_model_profile_settings(const ::Json::Value& config)
    {
        return valid_model(config, false);
    }

    ::Json::Value selected_model(const ::Json::Value& document, const ::Json::Value& defaults,
                                 const ::Json::Value& fallback)
    {
        if (const auto* selected =
                meowlive::server::find_profile(document, document["selected_profile_id"]))
        {
            auto config = meowlive::server::with_profile_defaults(defaults, (*selected)["config"]);
            config["api_key"] = (*selected)["api_key"];
            return config;
        }
        return fallback;
    }

    ::Json::Value model_profile_snapshot(const ::Json::Value& document,
                                         const ::Json::Value& defaults, const ::Json::Value& active)
    {
        const auto selected = meowlive::server::selected_model(document, defaults, active);
        ::Json::Value result(::Json::objectValue);
        result["profiles"] = ::Json::Value(::Json::arrayValue);
        for (const auto& profile : document["profiles"])
        {
            auto config = meowlive::server::with_profile_defaults(defaults, profile["config"]);
            config["api_key"] = profile["api_key"];
            ::Json::Value entry(::Json::objectValue);
            entry["id"] = profile["id"];
            entry["name"] = profile["name"];
            entry["settings"] = public_model(config);
            entry["key_configured"] = has_model_key(config);
            result["profiles"].append(std::move(entry));
        }
        result["storage_available"] = true;
        result["selected_profile_id"] = document["selected_profile_id"];
        result["settings"] = public_model(selected);
        result["key_configured"] = has_model_key(selected);
        result["active_model"] = active["model"];
        result["restart_required"] = selected != active;
        return result;
    }

    std::expected<::Json::Value, meowlive::server::ProfileError>
    draft_model(const ::Json::Value& input, const ::Json::Value& old, bool listing)
    {
        const auto& settings = input["settings"];
        const auto& supplied = input["api_key"];
        if (!meowlive::server::exact_fields(settings, public_model_fields) ||
            !input["clear_api_key"].isBool() || (!supplied.isNull() && !supplied.isString()))
        {
            return std::unexpected(meowlive::server::ProfileError::Invalid);
        }
        for (const auto name : public_model_fields)
        {
            const bool required =
                listing ? name == "base_url" || name == "provider" || name == "api_format"
                        : name != "reasoning_effort";
            if (required && !settings.isMember(std::string(name)))
            {
                return std::unexpected(meowlive::server::ProfileError::Invalid);
            }
        }
        if (!settings["base_url"].isString() || (!listing && !settings["model"].isString()) ||
            (input["clear_api_key"].asBool() && !supplied.isNull()))
        {
            return std::unexpected(meowlive::server::ProfileError::Invalid);
        }
        auto config = meowlive::server::with_profile_defaults(old, settings);
        config["reasoning_effort"] = settings.isMember("reasoning_effort")
                                         ? settings["reasoning_effort"]
                                         : ::Json::Value("default");
        if (input["clear_api_key"].asBool() || !same_destination(old, config))
        {
            config["api_key"] = ::Json::Value{};
            config["api_key_env"] = "";
        }
        if (!supplied.isNull())
        {
            config["api_key"] = supplied;
            config["api_key_env"] = "";
        }
        if (config["mode"] == "local" || old["mode"] == "local")
        {
            config["max_retries"] = 0;
        }
        if (config["mode"] == "local")
        {
            config["api_key_env"] = "";
        }
        if (!valid_model(config, true, listing))
        {
            return std::unexpected(meowlive::server::ProfileError::Invalid);
        }
        return config;
    }

    std::expected<void, meowlive::server::ProfileError>
    edit_model(::Json::Value& document, meowlive::server::ProfileEdit edit,
               const ::Json::Value& input, const ::Json::Value& defaults,
               const ::Json::Value& active)
    {
        using Edit = meowlive::server::ProfileEdit;
        using Error = meowlive::server::ProfileError;
        const auto selection = document["selected_profile_id"];
        const auto& id = edit == Edit::Save ? selection : input["id"];
        if (edit == Edit::Save && input.isMember("id") && input["id"] != selection)
        {
            return std::unexpected(Error::Stale);
        }
        auto* profile = meowlive::server::find_profile(document, id);
        if (!profile && edit != Edit::Create && edit != Edit::Save)
        {
            return std::unexpected(Error::Invalid);
        }
        if (edit == Edit::Rename)
        {
            if (!meowlive::server::profile_detail::set_name(
                    document, *profile, input["name"],
                    meowlive::server::profile_detail::NameRule::Existing))
            {
                return std::unexpected(Error::Invalid);
            }
        }
        else if (edit == Edit::Delete)
        {
            meowlive::server::profile_detail::delete_profile(document, id);
        }
        else if (edit == Edit::Select)
        {
            document["selected_profile_id"] = id;
        }
        else
        {
            auto config = meowlive::server::draft_model(
                input, meowlive::server::selected_model(document, defaults, active));
            if (!config)
            {
                return std::unexpected(config.error());
            }
            ::Json::Value new_profile(::Json::objectValue);
            if (edit == Edit::Create || !profile)
            {
                if (document["profiles"].size() >= 32 ||
                    !meowlive::server::profile_detail::set_name(
                        document, new_profile,
                        edit == Edit::Save ? ::Json::Value("") : input["name"],
                        meowlive::server::profile_detail::NameRule::Create))
                {
                    return std::unexpected(Error::Invalid);
                }
                new_profile["id"] = drogon::utils::getUuid();
                profile = &new_profile;
            }
            (*profile)["api_key"] = (*config)["api_key"];
            config->removeMember("api_key");
            (*profile)["config"] = std::move(*config);
            if (profile == &new_profile)
            {
                document["selected_profile_id"] = new_profile["id"];
                document["profiles"].append(std::move(new_profile));
            }
        }
        return {};
    }
}
