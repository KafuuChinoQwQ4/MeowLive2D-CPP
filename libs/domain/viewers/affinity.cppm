module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.domain.affinity;

import meowlive.standard;

export namespace meowlive::domain
{
    int exchange_delta(std::int64_t used_today, bool first_award_for_viewer)
    {
        return first_award_for_viewer
                   ? static_cast<int>(
                         2'000 - std::clamp(used_today, std::int64_t{1'800}, std::int64_t{2'000}))
                   : 0;
    }

    int apply_affinity_delta(int current, int computed)
    {
        return static_cast<int>(std::clamp(static_cast<std::int64_t>(current) + computed,
                                           std::int64_t{0}, std::int64_t{100'000}));
    }

    // The event text has already been decoded as UTF-8.
    std::string normalized_chat_body(std::string_view body)
    {
        std::string normalized;
        normalized.reserve(body.size());
        bool gap{};
        for (std::size_t offset = 0; offset < body.size();)
        {
            const auto start = offset;
            const auto lead = static_cast<unsigned char>(body[offset++]);
            std::uint32_t code = lead;
            if (lead >= 0xc0)
            {
                code = lead & (lead < 0xe0 ? 0x1f : lead < 0xf0 ? 0x0f : 0x07);
                while (offset < body.size() &&
                       (static_cast<unsigned char>(body[offset]) & 0xc0) == 0x80)
                {
                    code = (code << 6) | (static_cast<unsigned char>(body[offset++]) & 0x3f);
                }
            }
            if ((code >= 9 && code <= 13) || code == 0x20 || code == 0x85 || code == 0xa0 ||
                code == 0x1680 || (code >= 0x2000 && code <= 0x200a) || code == 0x2028 ||
                code == 0x2029 || code == 0x202f || code == 0x205f || code == 0x3000)
            {
                gap = !normalized.empty();
                continue;
            }
            if (gap)
            {
                normalized.push_back(' ');
                gap = false;
            }
            normalized.append(body.substr(start, offset - start));
        }
        return normalized;
    }
}
