module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.domain.speech;

import meowlive.standard;

namespace
{
    bool whitespace(std::uint32_t value)
    {
        return (value >= 9 && value <= 13) || value == 0x20 || value == 0x85 || value == 0xa0 ||
               value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
               value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
    }
}

export namespace meowlive::domain
{
    enum class SpeechStatus
    {
        Queued,
        Synthesizing,
        Ready,
        Playing,
        Completed,
        Cancelled,
        Failed,
        Unknown,
    };

    // JSON has already validated the UTF-8 input at the transport boundary.
    std::expected<std::string, std::string_view> normalize_speech_text(std::string_view text,
                                                                       unsigned maximum = 500)
    {
        std::size_t first = text.size();
        std::size_t last{};
        unsigned count{};
        unsigned count_at_last{};
        bool control{};
        bool control_at_last{};
        for (std::size_t offset = 0; offset < text.size();)
        {
            const auto start = offset;
            const auto lead = static_cast<unsigned char>(text[offset++]);
            std::uint32_t value = lead;
            if (lead >= 0xc0)
            {
                value = lead & (lead < 0xe0 ? 0x1f : lead < 0xf0 ? 0x0f : 0x07);
                while (offset < text.size() &&
                       (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80)
                {
                    value = (value << 6) | (static_cast<unsigned char>(text[offset++]) & 0x3f);
                }
            }
            const bool space = whitespace(value);
            if (first == text.size() && space)
            {
                continue;
            }
            if (first == text.size())
            {
                first = start;
            }
            ++count;
            control = control || ((value < 0x20 || (value >= 0x7f && value <= 0x9f)) &&
                                  value != '\n' && value != '\r' && value != '\t');
            if (!space)
            {
                last = offset;
                count_at_last = count;
                control_at_last = control;
            }
        }
        if (!last)
        {
            return std::unexpected("speech text must not be empty");
        }
        if (count_at_last > maximum)
        {
            return std::unexpected(maximum == 500
                                       ? "speech text must contain at most 500 characters"
                                       : "speech broadcast exceeds its length limit");
        }
        if (control_at_last)
        {
            return std::unexpected("speech text contains a control character");
        }
        return std::string(text.substr(first, last - first));
    }

    bool valid_voice_id(std::string_view value)
    {
        if (value.empty() || value.size() > 64)
        {
            return false;
        }
        for (const char byte : value)
        {
            if (!(byte >= 'a' && byte <= 'z') && !(byte >= 'A' && byte <= 'Z') &&
                !(byte >= '0' && byte <= '9') && byte != '_' && byte != '-')
            {
                return false;
            }
        }
        return true;
    }
}
