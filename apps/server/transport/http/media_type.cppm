module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.media_type;

import meowlive.standard;

namespace
{
    bool token_char(char character)
    {
        constexpr std::string_view punctuation{"!#$%&'*+-.^_|~\x60"};
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || punctuation.contains(character);
    }

    void skip_spaces(std::string_view& text)
    {
        text.remove_prefix(std::min(text.find_first_not_of(' '), text.size()));
    }

    bool valid_parameters(std::string_view parameters)
    {
        skip_spaces(parameters);
        while (!parameters.empty())
        {
            const auto equals = parameters.find('=');
            if (equals == 0 || equals == std::string_view::npos ||
                !std::ranges::all_of(parameters.substr(0, equals), token_char))
            {
                return false;
            }
            parameters.remove_prefix(equals + 1);
            if (parameters.starts_with('"'))
            {
                parameters.remove_prefix(1);
                const auto quote = parameters.find('"');
                if (quote == 0 || quote == std::string_view::npos ||
                    !std::ranges::all_of(parameters.substr(0, quote),
                                         [](unsigned char character)
                                         {
                                             return character >= 0x20 && character <= 0x7e;
                                         }))
                {
                    return false;
                }
                parameters.remove_prefix(quote + 1);
                skip_spaces(parameters);
                if (parameters.empty())
                {
                    return true;
                }
                if (!parameters.starts_with(';'))
                {
                    return false;
                }
            }
            else
            {
                const auto next = parameters.find(';');
                if (next == 0 || !std::ranges::all_of(parameters.substr(0, next), token_char))
                {
                    return false;
                }
                if (next == std::string_view::npos)
                {
                    return true;
                }
                parameters.remove_prefix(next);
            }
            parameters.remove_prefix(1);
            skip_spaces(parameters);
        }
        return true;
    }
}

export namespace meowlive::server::transport
{
    bool is_json_media_type(std::string_view value)
    {
        const auto separator = value.find(';');
        std::string media_type(value.substr(0, separator));
        std::ranges::transform(media_type, media_type.begin(),
                               [](char character)
                               {
                                   return character >= 'A' && character <= 'Z'
                                              ? static_cast<char>(character - 'A' + 'a')
                                              : character;
                               });
        std::string_view subtype(media_type);
        constexpr std::string_view prefix{"application/"};
        if (!subtype.starts_with(prefix))
        {
            return false;
        }
        subtype.remove_prefix(prefix.size());
        auto suffix = subtype.rfind('+');
        if (suffix == 0)
        {
            suffix = std::string_view::npos;
        }
        if (subtype.substr(0, suffix) != "json" &&
            (suffix == std::string_view::npos || subtype.substr(suffix + 1) != "json"))
        {
            return false;
        }
        return std::ranges::all_of(subtype, token_char) &&
               (separator == std::string_view::npos ||
                valid_parameters(value.substr(separator + 1)));
    }
}
