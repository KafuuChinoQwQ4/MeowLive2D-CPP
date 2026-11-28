module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.profile_edits;

import meowlive.standard;

import meowlive.protocol.utf8;

namespace meowlive::server::profile_detail
{
    std::string_view trim(std::string_view value)
    {
        const auto first = value.find_first_not_of(" \t\r\n\v\f");
        if (first == std::string_view::npos)
        {
            return {};
        }
        return value.substr(first, value.find_last_not_of(" \t\r\n\v\f") - first + 1);
    }

    bool text(const ::Json::Value& value, std::size_t maximum, bool empty, bool multiline)
    {
        if (!value.isString())
        {
            return false;
        }
        const auto bytes = value.asString();
        if ((!empty && trim(bytes).empty()) || !meowlive::protocol::is_utf8(bytes))
        {
            return false;
        }
        std::size_t count{};
        unsigned char previous{};
        for (auto character : bytes)
        {
            const auto byte = static_cast<unsigned char>(character);
            if ((byte < 0x20 && !(multiline && (byte == '\n' || byte == '\t'))) || byte == 0x7f ||
                (previous == 0xc2 && byte >= 0x80 && byte <= 0x9f))
            {
                return false;
            }
            count += (byte & 0xc0) != 0x80;
            previous = byte;
        }
        return count <= maximum;
    }

    bool between(const ::Json::Value& value, std::uint64_t minimum, std::uint64_t maximum)
    {
        return (value.type() == ::Json::intValue || value.type() == ::Json::uintValue) &&
               value.isUInt64() && value.asUInt64() >= minimum && value.asUInt64() <= maximum;
    }

    bool one_of(const ::Json::Value& value, std::initializer_list<std::string_view> valid)
    {
        return value.isString() && std::ranges::find(valid, value.asString()) != valid.end();
    }

    std::string next_name(const ::Json::Value& document)
    {
        for (unsigned index = 1; index <= document["profiles"].size() + 1; ++index)
        {
            auto name = "配置" + std::to_string(index);
            if (std::ranges::none_of(document["profiles"],
                                     [&](const auto& profile)
                                     {
                                         return profile["name"] == name;
                                     }))
            {
                return name;
            }
        }
        return {};
    }

    bool set_name(const ::Json::Value& document, ::Json::Value& profile, const ::Json::Value& name,
                  NameRule rule)
    {
        if (!name.isString())
        {
            return false;
        }
        const auto input = name.asString();
        const auto value = trim(input);
        profile["name"] =
            value.empty() && rule == NameRule::Create ? next_name(document) : std::string(value);
        return text(profile["name"], 64, false);
    }

    void delete_profile(::Json::Value& document, const ::Json::Value& id)
    {
        auto& profiles = document["profiles"];
        for (::Json::ArrayIndex index = 0; index < profiles.size(); ++index)
        {
            if (profiles[index]["id"] != id)
            {
                continue;
            }
            profiles.removeIndex(index, nullptr);
            if (document["selected_profile_id"] == id)
            {
                document["selected_profile_id"] =
                    profiles.empty() ? ::Json::Value{} : profiles[0u]["id"];
            }
            return;
        }
    }
}
