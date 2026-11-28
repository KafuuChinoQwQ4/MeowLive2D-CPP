module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.trial_pcm;

import meowlive.standard;
import meowlive.application.speech;

export namespace meowlive::adapters
{
    meowlive::application::PcmAudio trial_pcm()
    {
        constexpr std::uint32_t sample_rate = 24'000;
        constexpr std::uint32_t frame_count = sample_rate * 4;
        constexpr std::array frequencies{900, 1700, 2500, 3900};

        meowlive::application::PcmAudio audio{sample_rate, 1, {}};
        audio.samples.reserve(frame_count);
        for (std::uint32_t frame = 0; frame < frame_count; ++frame)
        {
            double sum = 0.0;
            for (const auto frequency : frequencies)
            {
                sum += std::sin(2.0 * std::numbers::pi * frequency * frame / sample_rate);
            }
            audio.samples.push_back(static_cast<std::int16_t>(std::lround(1800.0 * sum)));
        }
        return audio;
    }

    class TrialSynthesizer final : public meowlive::application::SpeechSynthesizer
    {
    public:
        std::expected<meowlive::application::PcmAudio, std::string>
        synthesize(const meowlive::application::SynthesisRequest&, std::stop_token) override
        {
            return meowlive::adapters::trial_pcm();
        }
    };
}
