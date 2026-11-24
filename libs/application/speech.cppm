module;

#if defined(_WIN32)
#include <cstdint>
#include <string>
#include <vector>
#endif

export module meowlive.application.speech;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::application
{
    struct SynthesisRequest
    {
        std::string text;
        std::string voice_id;
    };

    struct PcmAudio
    {
        std::uint32_t sample_rate{};
        std::uint16_t channels{};
        std::vector<std::int16_t> samples;
    };
}
