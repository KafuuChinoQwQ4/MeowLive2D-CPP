module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.wav;

import meowlive.standard;
import meowlive.application.speech;
import meowlive.protocol.audio;

namespace
{
    constexpr auto invalid_wav = "speech engine returned invalid or truncated WAV audio";

    std::uint16_t little16(std::string_view bytes)
    {
        return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]) |
                                          (static_cast<unsigned char>(bytes[1]) << 8));
    }

    std::uint32_t little32(std::string_view bytes)
    {
        return static_cast<std::uint32_t>(little16(bytes)) |
               (static_cast<std::uint32_t>(little16(bytes.substr(2))) << 16);
    }

    bool is_pcm16(std::string_view format)
    {
        constexpr std::string_view pcm_guid{"\x01\x00\x00\x00\x00\x00\x10\x00"
                                            "\x80\x00\x00\xaa\x00\x38\x9b\x71",
                                            16};
        const auto encoding = little16(format);
        return little16(format.substr(14)) == 16 &&
               (encoding == 1 ||
                (encoding == 0xfffe && format.size() >= 40 && little16(format.substr(16)) >= 22 &&
                 little16(format.substr(16)) <= format.size() - 18 &&
                 little16(format.substr(18)) == 16 && format.substr(24, 16) == pcm_guid));
    }
}

export namespace meowlive::adapters
{
    std::expected<meowlive::application::PcmAudio, std::string> decode_wav(std::string_view bytes)
    {
        if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE" ||
            little32(bytes.substr(4)) != bytes.size() - 8)
        {
            return std::unexpected(invalid_wav);
        }

        std::string_view format;
        std::string_view data;
        bytes.remove_prefix(12);
        while (!bytes.empty())
        {
            if (bytes.size() < 8)
            {
                return std::unexpected(invalid_wav);
            }
            const auto kind = bytes.substr(0, 4);
            const auto size = little32(bytes.substr(4));
            bytes.remove_prefix(8);
            if (static_cast<std::uint64_t>(size) + size % 2 > bytes.size())
            {
                return std::unexpected(invalid_wav);
            }

            if (kind == "fmt ")
            {
                if (!format.empty() || size < 16)
                {
                    return std::unexpected(invalid_wav);
                }
                format = bytes.substr(0, size);
            }
            else if (kind == "data")
            {
                if (!data.empty() || size == 0)
                {
                    return std::unexpected(invalid_wav);
                }
                data = bytes.substr(0, size);
            }
            bytes.remove_prefix(static_cast<std::size_t>(size) + size % 2);
        }

        if (format.empty() || data.empty())
        {
            return std::unexpected(invalid_wav);
        }
        if (!is_pcm16(format))
        {
            return std::unexpected("speech engine WAV must contain signed PCM16");
        }

        const auto channels = little16(format.substr(2));
        const auto sample_rate = little32(format.substr(4));
        const auto bytes_per_frame = little16(format.substr(12));
        if (!meowlive::protocol::valid_audio_format({sample_rate, channels}) ||
            bytes_per_frame != channels * 2 ||
            little32(format.substr(8)) != sample_rate * bytes_per_frame)
        {
            return std::unexpected("speech engine WAV has an unsupported audio format");
        }
        if (data.size() % bytes_per_frame != 0)
        {
            return std::unexpected("speech engine WAV must contain complete audio frames");
        }

        meowlive::application::PcmAudio audio{sample_rate, channels, {}};
        audio.samples.reserve(data.size() / 2);
        for (std::size_t offset = 0; offset < data.size(); offset += 2)
        {
            audio.samples.push_back(std::bit_cast<std::int16_t>(little16(data.substr(offset))));
        }
        return audio;
    }

    std::expected<std::string, std::string> encode_wav(const meowlive::application::PcmAudio& audio)
    {
        if (!meowlive::protocol::valid_audio_format({audio.sample_rate, audio.channels}) ||
            audio.samples.empty() || audio.samples.size() > 16 * 1024 * 1024 ||
            audio.samples.size() % audio.channels != 0)
        {
            return std::unexpected(std::string("播报音频超出 WAV 格式范围"));
        }
        const auto data_size = static_cast<std::uint32_t>(audio.samples.size() * 2);
        std::string bytes;
        bytes.reserve(44 + data_size);
        const auto integer = [&bytes](std::uint32_t number, unsigned width)
        {
            for (unsigned offset = 0; offset < width; ++offset)
            {
                bytes.push_back(static_cast<char>((number >> (offset * 8)) & 0xff));
            }
        };
        bytes.append("RIFF");
        integer(36 + data_size, 4);
        bytes.append("WAVEfmt ");
        integer(16, 4);
        integer(1, 2);
        integer(audio.channels, 2);
        integer(audio.sample_rate, 4);
        integer(audio.sample_rate * audio.channels * 2, 4);
        integer(audio.channels * 2, 2);
        integer(16, 2);
        bytes.append("data");
        integer(data_size, 4);
        for (const auto sample : audio.samples)
        {
            integer(std::bit_cast<std::uint16_t>(sample), 2);
        }
        return bytes;
    }
}
