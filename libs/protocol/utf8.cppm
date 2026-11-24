module;

#if defined(_WIN32)
#include <cstdint>
#include <string_view>
#endif

export module meowlive.protocol.utf8;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::protocol
{
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
