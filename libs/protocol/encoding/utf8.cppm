module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.utf8;

import meowlive.standard;

export namespace meowlive::protocol
{
    std::string_view trim_utf8_whitespace(std::string_view text)
    {
        constexpr std::array<std::string_view, 25> whitespace{
            " ",      "\t",     "\n",     "\v",     "\f",     "\r",     "\u0085",
            "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002", "\u2003", "\u2004",
            "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a", "\u2028",
            "\u2029", "\u202f", "\u205f", "\u3000"};
        for (const bool beginning : {true, false})
        {
            while (!text.empty())
            {
                const auto space = std::ranges::find_if(whitespace,
                                                        [&](std::string_view item)
                                                        {
                                                            return beginning
                                                                       ? text.starts_with(item)
                                                                       : text.ends_with(item);
                                                        });
                if (space == whitespace.end())
                {
                    break;
                }
                if (beginning)
                {
                    text.remove_prefix(space->size());
                }
                else
                {
                    text.remove_suffix(space->size());
                }
            }
        }
        return text;
    }

    bool is_utf8(std::string_view bytes)
    {
        std::uint32_t code_point{};
        std::uint32_t minimum{};
        int remaining{};
        for (const auto value : bytes)
        {
            const auto byte = static_cast<unsigned char>(value);
            if (remaining != 0)
            {
                if ((byte & 0xc0) != 0x80)
                {
                    return false;
                }
                code_point = (code_point << 6) | (byte & 0x3f);
                if (--remaining == 0 && (code_point < minimum || code_point > 0x10ffff ||
                                         (code_point >= 0xd800 && code_point <= 0xdfff)))
                {
                    return false;
                }
            }
            else if (byte >= 0xf0 && byte <= 0xf4)
            {
                code_point = byte & 0x07;
                minimum = 0x10000;
                remaining = 3;
            }
            else if (byte >= 0xe0 && byte <= 0xef)
            {
                code_point = byte & 0x0f;
                minimum = 0x800;
                remaining = 2;
            }
            else if (byte >= 0xc2 && byte <= 0xdf)
            {
                code_point = byte & 0x1f;
                minimum = 0x80;
                remaining = 1;
            }
            else if (byte >= 0x80)
            {
                return false;
            }
        }
        return remaining == 0;
    }
}
