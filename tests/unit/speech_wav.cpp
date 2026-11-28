#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.adapters.speech.wav;
import meowlive.application.speech;

namespace
{
    using std::string_view_literals::operator""sv;

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    std::string chunk_header(std::string_view type, std::uint32_t size)
    {
        std::string header(type);
        for (unsigned int byte = 0; byte < 4; ++byte)
        {
            header.push_back(static_cast<char>((size >> (8 * byte)) & 0xff));
        }
        return header;
    }

    std::string chunk(std::string_view type, std::string_view bytes)
    {
        auto result = chunk_header(type, static_cast<std::uint32_t>(bytes.size()));
        result.append(bytes);
        if (bytes.size() % 2 != 0)
        {
            result.push_back('\0');
        }
        return result;
    }

    std::string riff(std::string_view chunks)
    {
        auto result = chunk_header("RIFF", static_cast<std::uint32_t>(chunks.size() + 4));
        result.append("WAVE").append(chunks);
        return result;
    }

    void check_wav()
    {
        constexpr auto pcm16_stereo_24khz =
            "\x01\x00\x02\x00\xc0\x5d\x00\x00\x00\x77\x01\x00\x04\x00\x10\x00"sv;
        constexpr auto extensible_stereo_24khz =
            "\xfe\xff\x02\x00\xc0\x5d\x00\x00\x00\x77\x01\x00\x04\x00\x10\x00"
            "\x16\x00\x10\x00\x03\x00\x00\x00\x01\x00\x00\x00\x00\x00\x10\x00"
            "\x80\x00\x00\xaa\x00\x38\x9b\x71"sv;
        constexpr auto stereo_samples = "\x00\x80\xff\x7f\xff\xff\x00\x00"sv;

        const auto format = chunk("fmt ", pcm16_stereo_24khz);
        const auto samples = chunk("data", stereo_samples);
        const auto padded_metadata = chunk("JUNK", "abc");
        const auto wav = riff(format + padded_metadata + samples + padded_metadata);
        const std::vector<std::int16_t> expected{-32768, 32767, -1, 0};
        const auto audio = meowlive::adapters::decode_wav(wav);
        require(audio && audio->sample_rate == 24'000 && audio->channels == 2 &&
                    audio->samples == expected,
                "PCM16 preserves signed samples and skips padded metadata");

        const auto extensible = chunk("fmt ", extensible_stereo_24khz);
        const auto result = meowlive::adapters::decode_wav(riff(extensible + samples));
        require(result && result->samples == expected, "decodes extensible PCM16");

        require(!meowlive::adapters::decode_wav(riff(format + samples + samples)),
                "rejects duplicate data chunks");
        require(!meowlive::adapters::decode_wav(riff(format + format + samples)),
                "rejects duplicate format chunks");
        require(!meowlive::adapters::decode_wav(
                    riff(format + chunk("data", stereo_samples.substr(0, 6)))),
                "rejects an incomplete stereo frame");
        require(
            !meowlive::adapters::decode_wav(riff(format + samples + padded_metadata.substr(0, 11))),
            "requires padding on an odd trailing chunk");
        require(!meowlive::adapters::decode_wav(wav.substr(0, wav.size() - 1)),
                "rejects a RIFF length mismatch");
    }
}

int main()
{
    try
    {
        check_wav();
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "{}", error.what());
        return 1;
    }
}
