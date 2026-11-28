module;

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.speech_voices;

import meowlive.standard;
import meowlive.adapters.speech.trial_pcm;

import meowlive.adapters.speech.gpt_sovits;
import meowlive.adapters.speech.model_weights;
import meowlive.adapters.storage.voice_catalog;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.domain.speech;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.configuration;

namespace
{
    std::optional<::Json::Value> resource_file(const std::filesystem::path& directory,
                                               std::string_view filename, std::size_t limit)
    {
        const auto path = directory / filename;
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (!exists && !error)
        {
            return {};
        }
        std::ifstream stream(path, std::ios::binary);
        std::string data(limit + 1, '\0');
        stream.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (stream.eof() && !stream.bad() && stream.gcount() <= static_cast<std::streamsize>(limit))
        {
            data.resize(static_cast<std::size_t>(stream.gcount()));
            if (auto parsed = meowlive::protocol::json::parse_object(data))
            {
                return parsed;
            }
        }
        throw std::runtime_error("播报设置无效 [resources/" + std::string(filename) + "]");
    }
}

namespace
{
    meowlive::adapters::GptSovitsConfig
    synthesis_config(const meowlive::server::SpeechConfig& speech)
    {
        meowlive::adapters::GptSovitsConfig common{
            .base_url = speech.base_url,
            .reference_audio = speech.reference_audio,
            .prompt_text = speech.prompt_text,
            .prompt_language = speech.prompt_language,
            .text_language = speech.text_language,
            .timeout = std::chrono::seconds(speech.timeout_seconds),
            .max_audio_bytes = speech.max_audio_bytes,
            .sentence_batch_size = speech.sentence_batch_size,
        };

        if (const auto stored =
                resource_file(speech.resources_directory, "speech-settings.json", 4096))
        {
            const auto& batch = (*stored)["sentence_batch_size"];
            if (stored->size() != 1 || !meowlive::protocol::json::is_unsigned_32(batch) ||
                batch.asUInt() < 1 || batch.asUInt() > 16)
            {
                throw std::runtime_error("播报设置无效 [resources/speech-settings.json]");
            }
            common.sentence_batch_size = batch.asUInt();
        }

        return common;
    }
}

export namespace meowlive::server
{
    enum class VoiceEditError
    {
        Invalid,
        Busy,
        Storage,
    };

    struct VoiceEditFailure
    {
        meowlive::server::VoiceEditError error;
        std::string_view detail;
    };

    class VoiceLibrary
    {
    public:
        using Result = std::expected<::Json::Value, meowlive::server::VoiceEditFailure>;
        using WeightSelection =
            std::function<std::expected<std::optional<meowlive::adapters::ModelWeights>,
                                        std::string>(std::string_view, std::stop_token)>;

        explicit VoiceLibrary(const meowlive::server::SpeechConfig& config,
                              std::shared_ptr<meowlive::adapters::ManagedModelWeights> control = {},
                              meowlive::server::VoiceLibrary::WeightSelection selection = {})
            : common_(
                  [&]
                  {
                      auto settings = synthesis_config(config);
                      settings.managed_weights = std::move(control);
                      settings.select_weights = std::move(selection);
                      return settings;
                  }()),
              catalog_(config.resources_directory, !common_.reference_audio.empty())
        {
        }

        meowlive::application::SpeechVoices voices() const
        {
            return build_voices(catalog_.snapshot());
        }

        std::expected<meowlive::application::PcmAudio, std::string>
        audition(std::string voice_id, std::string text,
                 const meowlive::adapters::ModelWeights& weights, std::stop_token stop) const
        {
            const auto catalog = catalog_.snapshot();
            auto config = common_;
            if (!config.managed_weights)
            {
                return std::unexpected(std::string("独立语音引擎尚未配置"));
            }
            if (voice_id != "default")
            {
                const auto& entries = catalog["voices"];
                const auto voice =
                    std::ranges::find_if(entries,
                                         [&voice_id](const auto& entry)
                                         {
                                             return entry["id"].asString() == voice_id;
                                         });
                if (voice == entries.end() || !this->configure_voice(config, *voice))
                {
                    return std::unexpected(std::string("关联音色不可用"));
                }
            }
            config.select_weights = [&weights](std::string_view, std::stop_token)
                -> std::expected<std::optional<meowlive::adapters::ModelWeights>, std::string>
            {
                return weights;
            };
            config.stop_weight_load = stop;
            return meowlive::adapters::GptSovits(std::move(config))
                .synthesize({std::move(text), std::move(voice_id)}, stop);
        }

        bool voice_available(std::string_view id) const
        {
            if (id == "default")
            {
                return !common_.reference_audio.empty();
            }
            const auto& voices = catalog_.snapshot()["voices"];
            return std::any_of(voices.begin(), voices.end(),
                               [&](const auto& voice)
                               {
                                   return voice["id"].asString() == id &&
                                          catalog_.reference_available(
                                              voice["reference"].asString());
                               });
        }

        Result edit_characters(meowlive::application::SpeechWorker& playback,
                               const std::function<Result(::Json::Value&)>& update,
                               std::string_view transaction,
                               const std::function<Result(const ::Json::Value&)>& confirm = {})
        {
            return edit(playback, update, transaction, {}, confirm);
        }

        // Called on the server's settings work queue.
        ::Json::Value snapshot() const
        {
            const auto& catalog = catalog_.snapshot();
            ::Json::Value result(::Json::objectValue);
            result["default_voice_available"] = !common_.reference_audio.empty();
            result["active_voice_id"] = active_voice(catalog);
            result["active_character_id"] = catalog["active_character_id"];
            result["characters"] = catalog["characters"].isArray()
                                       ? catalog["characters"]
                                       : ::Json::Value(::Json::arrayValue);
            auto& voices = result["voices"] = ::Json::Value(::Json::arrayValue);
            for (const auto& profile : catalog["voices"])
            {
                const auto available =
                    catalog_.reference_available(profile["reference"].asString());
                ::Json::Value entry(::Json::objectValue);
                for (const auto* field : {"id", "name", "language", "reference_text", "duration_ms",
                                          "sample_rate", "channels"})
                {
                    entry[field] = profile[field];
                }
                entry["available"] = available;
                entry["error"] = available ? ::Json::Value{} : ::Json::Value("参考 WAV 不可用");
                voices.append(std::move(entry));
            }
            return result;
        }

        Result import_voice(meowlive::application::SpeechWorker& playback, ::Json::Value metadata,
                            std::string id, std::string_view wav)
        {
            auto profile =
                meowlive::adapters::storage::imported_voice(std::move(metadata), id, wav);
            if (!profile)
            {
                return failure(meowlive::server::VoiceEditError::Invalid, profile.error());
            }
            return edit(
                playback,
                [&](::Json::Value& candidate)
                {
                    if (candidate["voices"].size() >= 64)
                    {
                        return failure(meowlive::server::VoiceEditError::Invalid, "音色库已满");
                    }
                    candidate["voices"].append(std::move(*profile));
                    if (!catalog_.save_reference(id, wav))
                    {
                        return failure(meowlive::server::VoiceEditError::Storage,
                                       "无法保存参考 WAV");
                    }
                    return Result{};
                },
                id, id);
        }

        Result select_voice(meowlive::application::SpeechWorker& playback, const std::string& id,
                            std::string_view transaction)
        {
            return edit(
                playback,
                [&](::Json::Value& candidate)
                {
                    const auto selected = find_voice(candidate["voices"], id);
                    if (!(id == "default" && !common_.reference_audio.empty()) &&
                        (selected == candidate["voices"].end() ||
                         !catalog_.reference_available((*selected)["reference"].asString())))
                    {
                        return failure(meowlive::server::VoiceEditError::Invalid, "音色不可用");
                    }
                    candidate["active_voice_id"] = id;
                    return Result{};
                },
                transaction);
        }

        Result delete_voice(meowlive::application::SpeechWorker& playback, const std::string& id,
                            std::string_view transaction)
        {
            std::string reference;
            auto result = edit(
                playback,
                [&](::Json::Value& candidate)
                {
                    auto& voices = candidate["voices"];
                    const auto item = find_voice(voices, id);
                    if (item == voices.end())
                    {
                        if (id.size() == 36 && !used_reference(voices, id))
                        {
                            reference = id;
                            return Result{};
                        }
                        return failure(meowlive::server::VoiceEditError::Invalid, "音色不存在");
                    }
                    reference = (*item)["reference"].asString();
                    ::Json::Value removed;
                    voices.removeIndex(item.index(), &removed);
                    if (used_reference(voices, reference))
                    {
                        reference.clear();
                    }
                    if (candidate["active_voice_id"] == id)
                    {
                        candidate["active_voice_id"] = "";
                    }
                    if (candidate["characters"].isArray())
                    {
                        for (auto& character : candidate["characters"])
                        {
                            if (character.isObject() && character["voice_id"] == id)
                            {
                                character["voice_id"] = "";
                                if (candidate["active_character_id"] == character["id"])
                                {
                                    candidate["active_character_id"] = ::Json::Value{};
                                }
                            }
                        }
                    }
                    return Result{};
                },
                transaction);
            if (result && !reference.empty() && !catalog_.remove_reference(reference))
            {
                return failure(meowlive::server::VoiceEditError::Storage,
                               "音色目录已更新，参考 WAV 清理失败");
            }
            return result;
        }

    private:
        static ::Json::Value::iterator find_voice(::Json::Value& voices, const std::string& id)
        {
            return std::find_if(voices.begin(), voices.end(),
                                [&id](const auto& voice)
                                {
                                    return voice["id"] == id;
                                });
        }

        static bool used_reference(const ::Json::Value& voices, const std::string& reference)
        {
            return std::any_of(voices.begin(), voices.end(),
                               [&reference](const auto& voice)
                               {
                                   return voice["reference"] == reference;
                               });
        }

        static Result failure(meowlive::server::VoiceEditError error, std::string_view detail)
        {
            return std::unexpected(meowlive::server::VoiceEditFailure{error, detail});
        }

        std::string active_voice(const ::Json::Value& catalog) const
        {
            auto id = catalog["active_voice_id"].asString();
            return id == "default" && common_.reference_audio.empty() ? "" : id;
        }

        meowlive::application::SpeechVoices build_voices(const ::Json::Value& catalog) const
        {
            meowlive::application::SpeechVoices voices;
            voices.active = active_voice(catalog);
            voices.entries.emplace("preview",
                                   std::make_shared<meowlive::adapters::TrialSynthesizer>());
            voices.entries.emplace("default",
                                   std::make_shared<meowlive::adapters::GptSovits>(common_));
            for (const auto& entry : catalog["voices"])
            {
                auto voice = common_;
                if (!this->configure_voice(voice, entry))
                {
                    continue;
                }
                voices.entries.emplace(
                    entry["id"].asString(),
                    std::make_shared<meowlive::adapters::GptSovits>(std::move(voice)));
            }
            return voices;
        }

        bool configure_voice(meowlive::adapters::GptSovitsConfig& voice,
                             const ::Json::Value& entry) const
        {
            const auto reference = entry["reference"].asString();
            if (!catalog_.reference_available(reference))
            {
                return false;
            }
            voice.reference_audio = catalog_.reference_path(reference);
            voice.prompt_text = entry["reference_text"].asString();
            voice.prompt_language = entry["language"].asString();
            voice.text_language = voice.prompt_language;
            return true;
        }

        void discard_reference(std::string_view reference) const noexcept
        {
            if (reference.empty())
            {
                return;
            }
            try
            {
                if (!catalog_.remove_reference(reference))
                {
                    LOG_ERROR << "speech_reference_cleanup_failed";
                }
            }
            catch (...)
            {
                meowlive::platform::log_current_exception(
                    "speech.reference.cleanup", meowlive::platform::ExceptionDetails::Omit);
            }
        }

        // Returning true keeps the newly persisted catalog and its corresponding speech voices.
        bool confirm_character_edit(const std::function<Result(const ::Json::Value&)>& confirm,
                                    ::Json::Value previous, std::string_view transaction,
                                    Result& result)
        {
            std::exception_ptr confirmation_error;
            try
            {
                result = confirm(catalog_.snapshot());
                if (result)
                {
                    return true;
                }
            }
            catch (...)
            {
                confirmation_error = std::current_exception();
                meowlive::platform::log_current_exception(
                    "character.confirm", meowlive::platform::ExceptionDetails::Omit);
            }

            bool restored = false;
            try
            {
                restored = catalog_.save(std::move(previous), transaction);
            }
            catch (...)
            {
                meowlive::platform::log_current_exception(
                    "character.rollback", meowlive::platform::ExceptionDetails::Omit);
            }
            if (!restored)
            {
                result = failure(meowlive::server::VoiceEditError::Storage,
                                 "无法撤销已保存的角色选择，请重试加载该角色");
                return true;
            }

            try
            {
                if (!confirm(catalog_.snapshot()))
                {
                    LOG_WARN << "previous_character_restore_unconfirmed";
                }
            }
            catch (...)
            {
                meowlive::platform::log_current_exception(
                    "character.restore", meowlive::platform::ExceptionDetails::Omit);
            }
            if (confirmation_error)
            {
                std::rethrow_exception(confirmation_error);
            }
            return false;
        }

        template <typename Update>
        Result edit(meowlive::application::SpeechWorker& playback, Update update,
                    std::string_view transaction, std::string_view new_reference = {},
                    const std::function<Result(const ::Json::Value&)>& confirm = {})
        {
            Result result;
            if (!playback.edit_voices(
                    [&]() -> std::optional<meowlive::application::SpeechVoices>
                    {
                        auto candidate = catalog_.snapshot();
                        result = update(candidate);
                        if (!result)
                        {
                            return {};
                        }
                        try
                        {
                            auto voices = build_voices(candidate);
                            auto previous = confirm ? catalog_.snapshot() : ::Json::Value{};
                            if (catalog_.save(std::move(candidate), transaction))
                            {
                                return !confirm ||
                                               confirm_character_edit(confirm, std::move(previous),
                                                                      transaction, result)
                                           ? std::optional(std::move(voices))
                                           : std::nullopt;
                            }
                        }
                        catch (...)
                        {
                            discard_reference(new_reference);
                            throw;
                        }
                        discard_reference(new_reference);
                        result =
                            failure(meowlive::server::VoiceEditError::Storage, "无法保存资源目录");
                        return {};
                    }))
            {
                return failure(meowlive::server::VoiceEditError::Busy,
                               "播报进行中或音色资源正在更新");
            }
            return result ? Result(snapshot()) : result;
        }

        const meowlive::adapters::GptSovitsConfig common_;
        meowlive::adapters::storage::VoiceCatalog catalog_;
    };
}
