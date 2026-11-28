module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.profile_edits;

import meowlive.standard;

import meowlive.protocol.utf8;

export namespace meowlive::server
{
    enum class ProfileEdit
    {
        Save,
        Create,
        Select,
        Rename,
        Update,
        Delete,
    };

    enum class ProfileError
    {
        Invalid,
        Stale,
        Storage,
    };

    bool exact_fields(const ::Json::Value& value, std::initializer_list<std::string_view> names)
    {
        return value.isObject() &&
               std::ranges::all_of(value.getMemberNames(),
                                   [&](const auto& name)
                                   {
                                       return std::ranges::find(names, name) != names.end();
                                   });
    }

    ::Json::Value with_profile_defaults(::Json::Value defaults, const ::Json::Value& values)
    {
        for (const auto& name : values.getMemberNames())
        {
            auto& field = defaults[name];
            field = values[name].isObject() && field.isObject()
                        ? meowlive::server::with_profile_defaults(field, values[name])
                        : values[name];
        }
        return defaults;
    }

    template <typename Json> auto* find_profile(Json& document, const ::Json::Value& id)
    {
        auto& profiles = document["profiles"];
        const auto profile = std::ranges::find_if(profiles,
                                                  [&](const auto& candidate)
                                                  {
                                                      return id.isString() && candidate["id"] == id;
                                                  });
        return profile == profiles.end() ? nullptr : &*profile;
    }
}

namespace meowlive::server::profile_detail
{
    enum class NameRule
    {
        Existing,
        Create,
    };

    std::string_view trim(std::string_view value);
    bool text(const ::Json::Value& value, std::size_t maximum, bool empty, bool multiline = false);
    bool between(const ::Json::Value& value, std::uint64_t minimum, std::uint64_t maximum);
    bool one_of(const ::Json::Value& value, std::initializer_list<std::string_view> valid);
    std::string next_name(const ::Json::Value& document);
    bool set_name(const ::Json::Value& document, ::Json::Value& profile, const ::Json::Value& name,
                  NameRule rule);
    void delete_profile(::Json::Value& document, const ::Json::Value& id);
}

export namespace meowlive::server
{
    bool valid_agent_profile_settings(const ::Json::Value& settings);
    std::expected<bool, meowlive::server::ProfileError>
    edit_persona(::Json::Value& document, meowlive::server::ProfileEdit edit,
                 const ::Json::Value& input, const ::Json::Value& defaults);
}

export namespace meowlive::server
{
    bool valid_model_profile_settings(const ::Json::Value& config);
    ::Json::Value selected_model(const ::Json::Value& document, const ::Json::Value& defaults,
                                 const ::Json::Value& fallback);
    ::Json::Value model_profile_snapshot(const ::Json::Value& document,
                                         const ::Json::Value& defaults,
                                         const ::Json::Value& active);
    std::expected<::Json::Value, meowlive::server::ProfileError>
    draft_model(const ::Json::Value& input, const ::Json::Value& old, bool listing = false);
    std::expected<void, meowlive::server::ProfileError>
    edit_model(::Json::Value& document, meowlive::server::ProfileEdit edit,
               const ::Json::Value& input, const ::Json::Value& defaults,
               const ::Json::Value& active);
}
