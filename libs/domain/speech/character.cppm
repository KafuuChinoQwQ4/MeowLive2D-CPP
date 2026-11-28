module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.domain.character;

import meowlive.standard;

namespace
{
    constexpr bool alphanumeric(unsigned char value)
    {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z');
    }
}

export namespace meowlive::domain
{
    bool valid_character_id(std::string_view id)
    {
        return !id.empty() && id.size() <= 64 &&
               std::ranges::all_of(id,
                                   [](unsigned char value)
                                   {
                                       return alphanumeric(value) || value == '-' || value == '_';
                                   });
    }

    bool valid_mouth_parameter(std::string_view parameter)
    {
        return parameter.size() >= 4 && parameter.size() <= 32 &&
               std::ranges::all_of(parameter, alphanumeric);
    }

    bool valid_character_text(std::string_view value, std::size_t limit)
    {
        if (value.find_first_not_of(" \t\r\n") == std::string_view::npos)
        {
            return false;
        }
        std::size_t characters{};
        for (const unsigned char byte : value)
        {
            if (byte < 0x20 || byte == 0x7f)
            {
                return false;
            }
            characters += (byte & 0xc0) != 0x80;
        }
        return characters <= limit;
    }
}
