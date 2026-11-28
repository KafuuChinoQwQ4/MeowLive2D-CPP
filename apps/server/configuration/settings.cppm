module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.settings;

import meowlive.standard;
import meowlive.protocol.json;

namespace
{
    [[noreturn]] void invalid(std::string_view file, std::string_view field)
    {
        throw std::runtime_error("本机设置无效 [" + std::string(file) + ": " + std::string(field) +
                                 "]");
    }

    std::optional<Json::Value> read_settings(const std::filesystem::path& config_path,
                                             std::string_view suffix, std::size_t limit)
    {
        auto filename = config_path.stem();
        filename += suffix;
        const auto path = config_path.parent_path() / "local" / filename;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found))
        {
            return std::nullopt;
        }
        if (error || !std::filesystem::is_regular_file(status))
        {
            invalid(suffix, "file");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            invalid(suffix, "file");
        }
        std::string bytes(limit + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.bad() || !input.eof() || input.gcount() > static_cast<std::streamsize>(limit))
        {
            invalid(suffix, "file");
        }
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        auto document = meowlive::protocol::json::parse_object(bytes);
        if (!document)
        {
            invalid(suffix, "JSON");
        }
        return document;
    }

    void check_fields(const Json::Value& value, std::string_view file, std::string_view field,
                      std::initializer_list<std::string_view> names)
    {
        if (!value.isObject())
        {
            invalid(file, field);
        }
        for (const auto& name : value.getMemberNames())
        {
            if (std::ranges::find(names, name) == names.end())
            {
                invalid(file, field);
            }
        }
    }

    bool is_unsigned(const Json::Value& value)
    {
        return (value.type() == Json::intValue || value.type() == Json::uintValue) &&
               value.isUInt64();
    }

    bool is_unsigned_between(const Json::Value& value, std::uint64_t minimum, std::uint64_t maximum)
    {
        return is_unsigned(value) && value.asUInt64() >= minimum && value.asUInt64() <= maximum;
    }

    bool is_optional_string(const Json::Value& value)
    {
        return value.isNull() || value.isString();
    }

    bool is_nonblank_string(const Json::Value& value)
    {
        return value.isString() &&
               value.asString().find_first_not_of(" \t\r\n\v\f") != std::string::npos;
    }

    Json::Value migrate_profile(Json::Value profile)
    {
        profile["id"] = "legacy";
        profile["name"] = "配置1";
        Json::Value profiles(Json::arrayValue);
        profiles.append(std::move(profile));
        return profiles;
    }

    const Json::Value* check_profiles(const Json::Value& document, std::string_view file)
    {
        const auto& profiles = document["profiles"];
        const auto& selected = document["selected_profile_id"];
        if (!profiles.isArray() || profiles.size() > 32)
        {
            invalid(file, "profiles");
        }
        if (!is_optional_string(selected))
        {
            invalid(file, "selected_profile_id");
        }
        std::set<std::string> ids;
        const Json::Value* active = nullptr;
        for (const auto& profile : profiles)
        {
            if (!profile.isObject() || !is_nonblank_string(profile["id"]) ||
                profile["id"].asString().size() > 64 ||
                !ids.insert(profile["id"].asString()).second)
            {
                invalid(file, "profiles[].id");
            }
            if (!is_nonblank_string(profile["name"]))
            {
                invalid(file, "profiles[].name");
            }
            if (profile["id"] == selected)
            {
                active = &profile;
            }
        }
        if (selected.isString() && !active)
        {
            invalid(file, "selected_profile_id");
        }
        return active;
    }

    void validate_interaction(const Json::Value& interaction, std::string_view file)
    {
        constexpr std::string_view field = "settings.interaction";
        check_fields(interaction, file, field,
                     {"chat_read_mode", "welcome_enabled", "busy_chat_count", "busy_enter_count",
                      "busy_pending_count", "welcome_cooldown_ms", "welcome_viewer_cooldown_ms"});
        if (interaction.isMember("chat_read_mode"))
        {
            const auto& mode = interaction["chat_read_mode"];
            if (!mode.isString() || (mode != "auto" && mode != "all" && mode != "selective"))
            {
                invalid(file, "settings.interaction.chat_read_mode");
            }
        }
        if (interaction.isMember("welcome_enabled") && !interaction["welcome_enabled"].isBool())
        {
            invalid(file, "settings.interaction.welcome_enabled");
        }
        struct Bounds
        {
            const char* name;
            std::uint64_t minimum;
            std::uint64_t maximum;
        };
        for (const auto& [name, minimum, maximum] :
             {Bounds{"busy_chat_count", 1, 1000}, Bounds{"busy_enter_count", 1, 1000},
              Bounds{"busy_pending_count", 1, 512}, Bounds{"welcome_cooldown_ms", 1000, 3600000},
              Bounds{"welcome_viewer_cooldown_ms", 1000, 86400000}})
        {
            if (interaction.isMember(name) &&
                !is_unsigned_between(interaction[name], minimum, maximum))
            {
                invalid(file, std::string(field) + "." + name);
            }
        }
    }

    void validate_agent_settings(const Json::Value& settings, std::string_view file)
    {
        check_fields(settings, file, "settings",
                     {"persona", "system_prompt", "topic", "proactive_enabled", "cooldown_ms",
                      "interaction"});
        if (!is_nonblank_string(settings["persona"]))
        {
            invalid(file, "settings.persona");
        }
        if (settings.isMember("system_prompt") && !settings["system_prompt"].isString())
        {
            invalid(file, "settings.system_prompt");
        }
        if (!settings["topic"].isString())
        {
            invalid(file, "settings.topic");
        }
        if (!settings["proactive_enabled"].isBool())
        {
            invalid(file, "settings.proactive_enabled");
        }
        if (!is_unsigned_between(settings["cooldown_ms"], 1000, 3600000))
        {
            invalid(file, "settings.cooldown_ms");
        }
        if (settings.isMember("interaction"))
        {
            validate_interaction(settings["interaction"], file);
        }
    }

    std::optional<Json::Value> load_agent(const std::filesystem::path& path)
    {
        constexpr std::string_view file = "-agent.json";
        auto document = read_settings(path, file, 512 * 1024);
        if (!document)
        {
            return std::nullopt;
        }
        const auto& schema = (*document)["schema"];
        if (!is_unsigned(schema) || (schema.asUInt64() != 1 && schema.asUInt64() != 2))
        {
            invalid(file, "schema");
        }
        const auto& settings = std::as_const(*document)["settings"];
        validate_agent_settings(settings, file);
        if (schema.asUInt64() == 1)
        {
            check_fields(*document, file, "JSON", {"schema", "settings"});
            Json::Value profile(Json::objectValue);
            profile["persona"] = settings["persona"];
            (*document)["profiles"] = migrate_profile(std::move(profile));
            (*document)["selected_profile_id"] = "legacy";
            (*document)["schema"] = 2;
        }
        check_fields(*document, file, "JSON",
                     {"schema", "settings", "selected_profile_id", "profiles"});
        const auto* active = check_profiles(*document, file);
        for (const auto& profile : (*document)["profiles"])
        {
            check_fields(profile, file, "profiles[]", {"id", "name", "persona"});
            if (!is_nonblank_string(profile["persona"]))
            {
                invalid(file, "profiles[].persona");
            }
        }
        if (active && (*active)["persona"] != settings["persona"])
        {
            invalid(file, "settings.persona");
        }
        return document;
    }

    void validate_llm_profile(const Json::Value& profile, std::string_view file)
    {
        const auto& config = profile["config"];
        check_fields(config, file, "profiles[].config",
                     {"mode", "provider", "api_format", "base_url", "model", "api_key_env",
                      "timeout_seconds", "max_response_bytes", "max_tokens", "json_mode",
                      "reasoning_effort", "max_retries"});
        for (const auto* field : {"mode", "provider", "api_format", "base_url", "model",
                                  "api_key_env", "reasoning_effort"})
        {
            if (config.isMember(field) && !config[field].isString())
            {
                invalid(file, std::string("profiles[].config.") + field);
            }
        }
        for (const auto* field :
             {"timeout_seconds", "max_response_bytes", "max_tokens", "max_retries"})
        {
            if (config.isMember(field) && !is_unsigned(config[field]))
            {
                invalid(file, std::string("profiles[].config.") + field);
            }
        }
        if (config.isMember("json_mode") && !config["json_mode"].isBool())
        {
            invalid(file, "profiles[].config.json_mode");
        }
        if (!is_optional_string(profile["api_key"]))
        {
            invalid(file, "profiles[].api_key");
        }
    }

    std::optional<Json::Value> load_llm(const std::filesystem::path& path)
    {
        constexpr std::string_view file = "-llm.json";
        auto document = read_settings(path, file, 128 * 1024);
        if (!document)
        {
            return std::nullopt;
        }
        const auto& schema = (*document)["schema"];
        if (!is_unsigned(schema) || (schema.asUInt64() != 1 && schema.asUInt64() != 2))
        {
            invalid(file, "schema");
        }
        if (schema.asUInt64() == 1)
        {
            check_fields(*document, file, "JSON", {"schema", "config", "api_key"});
            Json::Value profile(Json::objectValue);
            profile["config"] = std::move((*document)["config"]);
            profile["api_key"] = std::move((*document)["api_key"]);
            *document = Json::Value(Json::objectValue);
            (*document)["profiles"] = migrate_profile(std::move(profile));
            (*document)["selected_profile_id"] = "legacy";
            (*document)["schema"] = 2;
        }
        check_fields(*document, file, "JSON", {"schema", "selected_profile_id", "profiles"});
        check_profiles(*document, file);
        for (const auto& profile : (*document)["profiles"])
        {
            check_fields(profile, file, "profiles[]", {"id", "name", "config", "api_key"});
            validate_llm_profile(profile, file);
        }
        return document;
    }

    std::optional<Json::Value> load_live(const std::filesystem::path& path)
    {
        constexpr std::string_view file = "-live.json";
        auto document = read_settings(path, file, 16 * 1024);
        if (!document)
        {
            return std::nullopt;
        }
        check_fields(*document, file, "JSON", {"schema", "enabled", "app_id", "credentials"});
        const auto& schema = (*document)["schema"];
        if (!is_unsigned(schema) || schema.asUInt64() != 1)
        {
            invalid(file, "schema");
        }
        const auto& enabled = (*document)["enabled"];
        if (!enabled.isBool())
        {
            invalid(file, "enabled");
        }
        const auto& app_id = (*document)["app_id"];
        if (!is_unsigned(app_id) ||
            (enabled.asBool() && (app_id.asUInt64() == 0 ||
                                  app_id.asUInt64() > std::numeric_limits<std::int64_t>::max())))
        {
            invalid(file, "app_id");
        }
        const auto& credentials = (*document)["credentials"];
        check_fields(credentials, file, "credentials",
                     {"access_key_id", "access_key_secret", "identity_code"});
        for (const auto* field : {"access_key_id", "access_key_secret", "identity_code"})
        {
            const auto& value = credentials[field];
            if ((!value.isNull() || enabled.asBool()) && !is_nonblank_string(value))
            {
                invalid(file, std::string("credentials.") + field);
            }
        }
        return document;
    }
}

export namespace meowlive::server
{
    class ServerSettings
    {
    public:
        unsigned loaded_count() const noexcept
        {
            return static_cast<unsigned>(agent_.has_value()) +
                   static_cast<unsigned>(llm_.has_value()) +
                   static_cast<unsigned>(live_.has_value());
        }

        std::optional<::Json::Value> take_agent()
        {
            return std::move(agent_);
        }

        std::optional<::Json::Value> take_llm()
        {
            return std::move(llm_);
        }

        std::optional<::Json::Value> take_live()
        {
            return std::move(live_);
        }

    private:
        friend meowlive::server::ServerSettings
        load_settings(const std::filesystem::path& config_path);

        std::optional<Json::Value> agent_;
        std::optional<Json::Value> llm_;
        std::optional<Json::Value> live_;
    };

    meowlive::server::ServerSettings load_settings(const std::filesystem::path& config_path)
    {
        meowlive::server::ServerSettings settings;
        settings.agent_ = load_agent(config_path);
        settings.llm_ = load_llm(config_path);
        settings.live_ = load_live(config_path);
        return settings;
    }
}
