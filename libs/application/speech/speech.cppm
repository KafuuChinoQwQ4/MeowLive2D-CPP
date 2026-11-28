module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.speech;

import meowlive.standard;

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

    class SpeechSynthesizer
    {
    public:
        virtual ~SpeechSynthesizer() = default;

        virtual std::expected<meowlive::application::PcmAudio, std::string>
        synthesize(const meowlive::application::SynthesisRequest& request,
                   std::stop_token stop) = 0;
    };
}
