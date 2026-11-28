module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.live_settings;

import meowlive.standard;

import meowlive.application.live;
import meowlive.protocol.json;
import meowlive.server.configuration;
import meowlive.server.profile_file;

namespace
{
    using Credentials = meowlive::application::LiveCredentials;
    struct CredentialField
    {
        const char* name;
        const char* configured;
        std::string Credentials::* member;
        std::size_t limit;
    };

    constexpr std::array fields{CredentialField{"access_key_id", "access_key_id_configured",
                                                &Credentials::access_key_id, 256},
                                CredentialField{"access_key_secret", "access_key_secret_configured",
                                                &Credentials::access_key_secret, 512},
                                CredentialField{"identity_code", "identity_code_configured",
                                                &Credentials::identity_code, 512}};

    std::string_view trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r\n\v\f");
        return first == std::string_view::npos
                   ? std::string_view{}
                   : text.substr(first, text.find_last_not_of(" \t\r\n\v\f") - first + 1);
    }

    bool valid_credential(std::string_view value, std::size_t limit)
    {
        return value.size() <= limit && std::ranges::none_of(value,
                                                             [](unsigned char c)
                                                             {
                                                                 return c < 0x20 || c == 0x7f;
                                                             });
    }

    std::string environment(const std::string& name)
    {
        const auto* value = std::getenv(name.c_str());
        return value ? std::string(trim(value)) : std::string{};
    }
}

export namespace meowlive::server
{
    struct LiveSettings
    {
        bool enabled{};
        meowlive::application::LiveCredentials credentials;

        bool configured() const
        {
            return enabled && credentials.app_id != 0 &&
                   std::ranges::all_of(fields,
                                       [this](const CredentialField& field)
                                       {
                                           return !(credentials.*field.member).empty();
                                       });
        }

        ::Json::Value snapshot() const
        {
            ::Json::Value value(::Json::objectValue);
            value["enabled"] = enabled;
            value["app_id"] =
                credentials.app_id ? std::to_string(credentials.app_id) : std::string{};
            value["storage_available"] = true;
            for (const auto& field : fields)
            {
                value[field.configured] = !(credentials.*field.member).empty();
            }
            return value;
        }
    };

    meowlive::server::LiveSettings initial_live_settings(const meowlive::server::LiveConfig& config,
                                                         const std::optional<::Json::Value>& saved)
    {
        meowlive::server::LiveSettings result{.enabled = config.enabled,
                                              .credentials = {.app_id = config.app_id}};
        if (saved)
        {
            result.enabled = (*saved)["enabled"].asBool();
            result.credentials.app_id = (*saved)["app_id"].asUInt64();
            for (const auto& field : fields)
            {
                result.credentials.*field.member = (*saved)["credentials"][field.name].asString();
            }
        }
        else
        {
            result.credentials.access_key_id = environment(config.access_key_id_env);
            result.credentials.access_key_secret = environment(config.access_key_secret_env);
            result.credentials.identity_code = environment(config.identity_code_env);
        }
        for (const auto& field : fields)
        {
            auto& value = result.credentials.*field.member;
            if (!valid_credential(value, field.limit))
            {
                throw std::runtime_error(std::string("直播配置无效 [") + field.name + "]");
            }
        }
        return result;
    }

    std::expected<meowlive::server::LiveSettings, std::string>
    edit_live_settings(const meowlive::server::LiveSettings& current, const ::Json::Value& input)
    {
        if (!input["enabled"].isBool() || !input["app_id"].isString() ||
            (input.isMember("clear_credentials") && !input["clear_credentials"].isBool()))
        {
            return std::unexpected("直播配置字段无效");
        }
        for (const auto& name : input.getMemberNames())
        {
            if (name != "enabled" && name != "app_id" && name != "clear_credentials" &&
                std::ranges::none_of(fields,
                                     [&name](const auto& field)
                                     {
                                         return field.name == name;
                                     }))
            {
                return std::unexpected("直播配置含有未知字段");
            }
        }

        meowlive::server::LiveSettings candidate{.enabled = input["enabled"].asBool()};
        const auto raw_id = input["app_id"].asString();
        const auto id = trim(raw_id);
        auto& app_id = candidate.credentials.app_id;
        if (!id.empty())
        {
            const auto end = id.data() + id.size();
            const auto parsed = std::from_chars(id.data(), end, app_id);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                app_id > std::numeric_limits<std::int64_t>::max())
            {
                return std::unexpected("应用 ID 须为有效的十进制整数");
            }
        }
        const auto clear = input["clear_credentials"].asBool();
        if (clear && candidate.enabled)
        {
            return std::unexpected("清除凭据前请关闭直播接入");
        }
        for (const auto& field : fields)
        {
            const auto& input_value = input[field.name];
            if (!input_value.isNull() && !input_value.isString())
            {
                return std::unexpected("直播凭据字段无效");
            }
            const auto bytes = input_value.asString();
            const auto value = trim(bytes);
            if (!valid_credential(value, field.limit) || (clear && !value.empty()))
            {
                return std::unexpected("直播凭据字段无效");
            }
            if (!value.empty())
            {
                candidate.credentials.*field.member = value;
            }
            else if (!clear && app_id == current.credentials.app_id)
            {
                candidate.credentials.*field.member = current.credentials.*field.member;
            }
        }
        if (candidate.enabled && !candidate.configured())
        {
            return std::unexpected("请填写应用 ID、AccessKey ID、AccessKey Secret 和主播身份码");
        }
        return candidate;
    }

    bool save_live_settings(const std::filesystem::path& config_path,
                            const meowlive::server::LiveSettings& settings)
    {
        auto name = config_path.stem();
        name += "-live.json";
        const auto local = config_path.parent_path() / "local";
        ::Json::Value output(::Json::objectValue);
        output["schema"] = 1;
        output["enabled"] = settings.enabled;
        output["app_id"] = static_cast<::Json::UInt64>(settings.credentials.app_id);
        auto& credentials = output["credentials"];
        for (const auto& field : fields)
        {
            const auto& value = settings.credentials.*field.member;
            credentials[field.name] = value.empty() ? ::Json::Value{} : ::Json::Value(value);
        }
        return meowlive::server::write_profile_file(
            local / name, local / (".live-" + drogon::utils::getUuid() + ".tmp"),
            meowlive::protocol::json::serialize(output));
    }
}
