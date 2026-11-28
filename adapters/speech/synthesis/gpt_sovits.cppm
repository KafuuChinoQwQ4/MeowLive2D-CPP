module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.gpt_sovits;

import meowlive.standard;
import meowlive.application.speech;
import meowlive.adapters.speech.http;
import meowlive.adapters.speech.model_weights;
import meowlive.adapters.speech.wav;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

namespace
{
    bool is_language(std::string_view language)
    {
        return !language.empty() && language.size() <= 32 &&
               std::ranges::all_of(language,
                                   [](const char code)
                                   {
                                       return (code >= 'A' && code <= 'Z') ||
                                              (code >= 'a' && code <= 'z') || code == '_';
                                   });
    }

    std::expected<std::string, std::string> reference_path(const std::filesystem::path& path)
    {
        constexpr auto unavailable = "speech reference audio is missing or unreadable";
        if (path.empty())
        {
            return std::unexpected("speech reference audio is not configured");
        }
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error || !std::filesystem::is_regular_file(absolute, error) || error ||
            !std::ifstream(absolute, std::ios::binary))
        {
            return std::unexpected(unavailable);
        }

        const auto encoded = absolute.u8string();
        std::string text(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        if (text.size() > 4096 || !meowlive::protocol::is_utf8(text))
        {
            return std::unexpected(
                "speech reference audio path exceeds the request size limit or is not UTF-8");
        }
        return text;
    }
}

export namespace meowlive::adapters
{
    struct GptSovitsConfig
    {
        std::string base_url{"http://127.0.0.1:9880"};
        std::filesystem::path reference_audio;
        std::string prompt_text;
        std::string prompt_language{"zh"};
        std::string text_language{"zh"};
        std::chrono::seconds timeout{300};
        std::size_t max_audio_bytes = 32 * 1024 * 1024;
        unsigned int sentence_batch_size = 4;
        std::shared_ptr<meowlive::adapters::ManagedModelWeights> managed_weights;
        std::function<std::expected<std::optional<meowlive::adapters::ModelWeights>, std::string>(
            std::string_view, std::stop_token)>
            select_weights;
        std::stop_token stop_weight_load;
    };

    class GptSovits final : public meowlive::application::SpeechSynthesizer
    {
    public:
        explicit GptSovits(meowlive::adapters::GptSovitsConfig config) : config_(std::move(config))
        {
        }

        std::expected<meowlive::application::PcmAudio, std::string>
        synthesize(const meowlive::application::SynthesisRequest& request,
                   std::stop_token stop) override
        {
            if (stop.stop_requested())
            {
                return std::unexpected("speech synthesis cancelled");
            }
            if (request.text.empty() || !meowlive::protocol::is_utf8(request.text))
            {
                return std::unexpected("speech text is invalid");
            }
            if (!is_language(config_.prompt_language) || !is_language(config_.text_language) ||
                config_.sentence_batch_size < 1 || config_.sentence_batch_size > 16 ||
                config_.timeout.count() < 1 || config_.timeout.count() > 300 ||
                config_.max_audio_bytes < 1024 || config_.max_audio_bytes > 32 * 1024 * 1024 ||
                config_.prompt_text.size() > 8192 ||
                !meowlive::protocol::is_utf8(config_.prompt_text))
            {
                return std::unexpected("speech engine configuration is invalid");
            }
            const auto reference = reference_path(config_.reference_audio);
            if (!reference)
            {
                return std::unexpected(reference.error());
            }

            ::Json::Value body(::Json::objectValue);
            body["text"] = request.text;
            body["text_lang"] = config_.text_language;
            body["ref_audio_path"] = *reference;
            body["prompt_text"] = config_.prompt_text;
            body["prompt_lang"] = config_.prompt_language;
            body["text_split_method"] = "cut5";
            body["media_type"] = "wav";
            body["streaming_mode"] = false;
            body["batch_size"] = config_.sentence_batch_size;
            const auto payload = meowlive::protocol::json::serialize(body);
            if (payload.size() > 16 * 1024)
            {
                return std::unexpected("speech engine request exceeds the size limit");
            }

            if (config_.managed_weights && config_.select_weights)
            {
                const auto weights = config_.select_weights(request.voice_id, stop);
                if (!weights)
                {
                    return std::unexpected(weights.error());
                }
                if (!config_.managed_weights->select(*weights, config_.stop_weight_load))
                {
                    return std::unexpected("语音引擎无法载入成对权重");
                }
            }

            const auto response = meowlive::adapters::speech_http::post_tts(
                config_.base_url, payload, config_.timeout, config_.max_audio_bytes, stop);
            if (!response)
            {
                if (config_.managed_weights)
                {
                    config_.managed_weights->invalidate();
                }
                return std::unexpected(response.error());
            }
            auto audio = meowlive::adapters::decode_wav(*response);
            if (stop.stop_requested())
            {
                return std::unexpected("speech synthesis cancelled");
            }
            if (audio && std::ranges::all_of(audio->samples,
                                             [](const auto sample)
                                             {
                                                 return sample == 0;
                                             }))
            {
                return std::unexpected("speech engine returned silent audio");
            }
            return audio;
        }

    private:
        meowlive::adapters::GptSovitsConfig config_;
    };
}
