module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.voice_catalog;

import meowlive.standard;

import meowlive.adapters.speech.wav;
import meowlive.adapters.storage.atomic_file;
import meowlive.domain.speech;
import meowlive.protocol.json;

namespace
{
    constexpr std::size_t catalog_limit = 1024 * 1024;

    bool valid_resource_id(std::string_view id)
    {
        return meowlive::domain::valid_voice_id(id) && id != "default" && id != "preview" &&
               id != "active";
    }

    bool valid_text(std::string_view text, std::size_t limit, bool multiline)
    {
        if (text.find_first_not_of(" \t\r\n") == std::string_view::npos)
        {
            return false;
        }
        std::size_t characters = 0;
        for (const unsigned char character : text)
        {
            if (character < 32 || character == 127)
            {
                if (!multiline || (character != '\t' && character != '\r' && character != '\n'))
                {
                    return false;
                }
            }
            if ((character & 0xc0) != 0x80)
            {
                ++characters;
            }
        }
        return characters <= limit;
    }

    bool valid_metadata(const ::Json::Value& voice)
    {
        constexpr std::array<std::string_view, 6> languages{"zh", "en", "ja", "ko", "yue", "auto"};
        return voice["name"].isString() && valid_text(voice["name"].asString(), 80, false) &&
               voice["language"].isString() &&
               std::ranges::find(languages, voice["language"].asString()) != languages.end() &&
               voice["reference_text"].isString() &&
               valid_text(voice["reference_text"].asString(), 500, true);
    }

    ::Json::Value empty_catalog()
    {
        ::Json::Value catalog(::Json::objectValue);
        catalog["schema_version"] = 1u;
        catalog["active_voice_id"] = "";
        catalog["active_character_id"] = ::Json::Value{};
        catalog["voices"] = ::Json::Value(::Json::arrayValue);
        catalog["characters"] = ::Json::Value(::Json::arrayValue);
        return catalog;
    }

    bool valid_catalog(const ::Json::Value& catalog)
    {
        const auto& schema = catalog["schema_version"];
        const auto& selected = catalog["active_voice_id"];
        const auto& voices = catalog["voices"];
        if (!meowlive::protocol::json::is_unsigned_32(schema) || schema.asUInt() != 1u ||
            !selected.isString() || !voices.isArray() || voices.size() > 64 ||
            (!catalog["characters"].isNull() && !catalog["characters"].isArray()))
        {
            return false;
        }
        std::unordered_set<std::string> ids;
        for (const auto& voice : voices)
        {
            if (!voice.isObject() || !voice["id"].isString() || !voice["reference"].isString() ||
                !valid_resource_id(voice["id"].asString()) ||
                !valid_resource_id(voice["reference"].asString()) ||
                !voice["reference_text"].isString() || !voice["language"].isString() ||
                !ids.insert(voice["id"].asString()).second)
            {
                return false;
            }
        }
        const auto active = selected.asString();
        return active.empty() || active == "default" || ids.contains(active);
    }
}

export namespace meowlive::adapters::storage
{
    inline constexpr std::size_t voice_audio_limit = 2 * 1024 * 1024;

    std::expected<::Json::Value, std::string_view>
    imported_voice(::Json::Value metadata, std::string id, std::string_view wav)
    {
        if (!metadata.isObject() || metadata.size() != 3 || !valid_metadata(metadata))
        {
            return std::unexpected("音色名称、语言或参考文本无效");
        }
        if (wav.size() > meowlive::adapters::storage::voice_audio_limit)
        {
            return std::unexpected("参考 WAV 不能超过 2 MiB");
        }
        const auto audio = meowlive::adapters::decode_wav(wav);
        if (!audio || audio->sample_rate < 8000 || audio->sample_rate > 48000 ||
            audio->channels > 2 ||
            std::ranges::all_of(audio->samples,
                                [](auto sample)
                                {
                                    return sample == 0;
                                }))
        {
            return std::unexpected("参考音频需要包含非静音的 8 至 48 kHz PCM16 WAV");
        }
        const auto frames = audio->samples.size() / audio->channels;
        if (frames < 3u * audio->sample_rate || frames > 10u * audio->sample_rate)
        {
            return std::unexpected("参考 WAV 时长必须在 3 至 10 秒之间");
        }
        metadata["id"] = id;
        metadata["reference"] = std::move(id);
        metadata["duration_ms"] = static_cast<unsigned>(1000u * frames / audio->sample_rate);
        metadata["sample_rate"] = audio->sample_rate;
        metadata["channels"] = audio->channels;
        return metadata;
    }

    class VoiceCatalog
    {
    public:
        VoiceCatalog(std::filesystem::path resources, bool default_available)
            : resources_(std::move(resources)), catalog_(read(default_available))
        {
        }

        const ::Json::Value& snapshot() const
        {
            return catalog_;
        }

        std::filesystem::path reference_path(std::string_view id) const
        {
            return resources_ / "references" / (std::string(id) + ".wav");
        }

        bool reference_available(std::string_view id) const
        {
            std::error_code error;
            return std::filesystem::is_directory(
                       std::filesystem::symlink_status(resources_ / "references", error)) &&
                   std::filesystem::is_regular_file(
                       std::filesystem::symlink_status(reference_path(id), error));
        }

        bool save(::Json::Value candidate, std::string_view transaction)
        {
            auto bytes = meowlive::protocol::json::serialize(candidate);
            if (bytes.size() > catalog_limit ||
                !meowlive::adapters::storage::write_atomic_file(
                    resources_ / "catalog.json",
                    resources_ / (".catalog-" + std::string(transaction) + ".tmp"), bytes))
            {
                return false;
            }
            catalog_ = std::move(candidate);
            return true;
        }

        bool save_reference(std::string_view id, std::string_view wav) const
        {
            const auto target = reference_path(id);
            std::error_code error;
            const auto existing = std::filesystem::symlink_status(target, error);
            if ((error && error != std::errc::no_such_file_or_directory) ||
                std::filesystem::exists(existing))
            {
                return false;
            }
            return meowlive::adapters::storage::write_atomic_file(
                target, target.parent_path() / ("." + std::string(id) + ".tmp"), wav);
        }

        bool remove_reference(std::string_view id) const
        {
            std::error_code error;
            const auto target = reference_path(id);
            const auto status = std::filesystem::symlink_status(target, error);
            if (error == std::errc::no_such_file_or_directory)
            {
                return true;
            }
            if (error ||
                (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status)))
            {
                return false;
            }
            if (!std::filesystem::is_directory(
                    std::filesystem::symlink_status(target.parent_path(), error)))
            {
                return false;
            }
            return std::filesystem::remove(target, error) && !error;
        }

    private:
        ::Json::Value read(bool default_available) const
        {
            const auto path = resources_ / "catalog.json";
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory)
            {
                auto catalog = empty_catalog();
                catalog["active_voice_id"] = default_available ? "default" : "";
                return catalog;
            }
            if (!error && std::filesystem::is_regular_file(status))
            {
                std::ifstream stream(path, std::ios::binary);
                std::string bytes(catalog_limit + 1, '\0');
                stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (stream.eof() && !stream.bad() &&
                    stream.gcount() <= static_cast<std::streamsize>(catalog_limit))
                {
                    bytes.resize(static_cast<std::size_t>(stream.gcount()));
                    if (auto catalog = meowlive::protocol::json::parse_object(bytes);
                        catalog && valid_catalog(*catalog))
                    {
                        return std::move(*catalog);
                    }
                }
            }
            throw std::runtime_error("播报设置无效 [resources/catalog.json]");
        }

        const std::filesystem::path resources_;
        ::Json::Value catalog_;
    };
}
