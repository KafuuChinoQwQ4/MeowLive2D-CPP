module;

#if defined(_WIN32)
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>
#endif

export module meowlive.adapters.speech.training;

#if defined(_WIN32)
import meowlive.standard;
import meowlive.adapters.speech.voice_components;
import meowlive.adapters.speech.wav;
import meowlive.adapters.speech.windows_process;
import meowlive.application.voice_models;
import meowlive.protocol.json;
import meowlive.protocol.utf8;

export namespace meowlive::adapters::speech
{
    struct Transcript
    {
        std::string language;
        std::string text;
    };
}

namespace
{
    namespace speech = meowlive::adapters::speech;
    using Failure = std::unexpected<std::string>;

    constexpr auto transcribe_python = LR"PY(
import json
import os
import sys
from contextlib import redirect_stdout
from pathlib import Path
os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"
os.environ["TOKENIZERS_PARALLELISM"] = "false"

def transcribe(model_path, audio_path, language):
    with redirect_stdout(sys.stderr):
        from faster_whisper import WhisperModel
        model = WhisperModel(model_path, device="cpu", compute_type="int8",
                             cpu_threads=2, num_workers=1, local_files_only=True)
        if (language not in model.supported_languages or
            model.hf_tokenizer.token_to_id(f"<|{language}|>") is None):
            return {"error":"language"}
        if Path(audio_path).stat().st_size > 2 * 1024 * 1024:
            return {"error":"audio"}
        segments, info = model.transcribe(audio_path, language=language, beam_size=5,
            vad_filter=True, condition_on_previous_text=False)
        return {"text":"".join(segment.text for segment in segments).strip(),
                "language":info.language}

try:
    result = transcribe(*sys.argv[1:])
except (ImportError, OSError, RuntimeError, ValueError):
    result = {"error":"recognizer"}
print(json.dumps(result, ensure_ascii=False), flush=True)
)PY";

    bool supported_language(std::string_view language)
    {
        constexpr std::array<std::string_view, 5> supported{"zh", "en", "ja", "ko", "yue"};
        return std::ranges::find(supported, language) != supported.end();
    }

    bool valid_transcript(std::string_view text)
    {
        if (text.empty() || text.size() > 2'000 || !meowlive::protocol::is_utf8(text))
        {
            return false;
        }
        std::size_t length{};
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const auto byte = static_cast<unsigned char>(text[index]);
            if (byte < 0x20 || byte == 0x7f || byte == '|' ||
                (byte == 0xc2 && index + 1 < text.size() &&
                 static_cast<unsigned char>(text[index + 1]) >= 0x80 &&
                 static_cast<unsigned char>(text[index + 1]) <= 0x9f))
            {
                return false;
            }
            if ((byte & 0xc0) != 0x80 && ++length > 500)
            {
                return false;
            }
        }
        return true;
    }

    Failure process_error(speech::ProcessError error)
    {
        switch (error)
        {
        case speech::ProcessError::Cancelled:
            return Failure("转写已取消");
        case speech::ProcessError::Timeout:
            return Failure("转写超时");
        default:
            return Failure("无法执行转写，请检查 Windows 转写环境");
        }
    }

    std::expected<speech::Transcript, std::string>
    transcribe_audio(const std::filesystem::path& interpreter, std::span<const std::wstring> args,
                     const std::filesystem::path& audio, std::string_view language,
                     std::stop_token stop, std::chrono::seconds timeout)
    {
        if (stop.stop_requested())
        {
            return process_error(speech::ProcessError::Cancelled);
        }

        std::error_code error;
        if (!audio.is_absolute() || !std::filesystem::is_regular_file(audio, error))
        {
            return Failure("参考录音不可读");
        }
        const auto modified = std::filesystem::last_write_time(audio, error);
        if (error)
        {
            return Failure("参考录音不可读");
        }
        const auto size = std::filesystem::file_size(audio, error);
        if (error || size == 0 || size > 2 * 1024 * 1024)
        {
            return Failure("参考录音大小无效（上限 2 MiB）");
        }
        {
            std::ifstream stream(audio, std::ios::binary);
            std::string wav(static_cast<std::size_t>(size), '\0');
            stream.read(wav.data(), static_cast<std::streamsize>(size));
            if (!stream || !meowlive::adapters::decode_wav(wav))
            {
                return Failure("参考录音不是有效 WAV");
            }
        }

        const auto result = speech::run_windows_process(
            interpreter, args, audio.parent_path(),
            std::clamp(timeout, std::chrono::seconds(1), std::chrono::seconds(300)), 8192, stop);
        if (!result)
        {
            return process_error(result.error());
        }
        if (std::filesystem::last_write_time(audio, error) != modified || error ||
            std::filesystem::file_size(audio, error) != size || error)
        {
            return Failure("参考录音已更换，请重新转写");
        }
        const auto data = meowlive::protocol::json::parse_object(*result);
        if (data && (*data)["error"] == "language")
        {
            return Failure("转写模型不支持所选语言");
        }
        if (data && (*data)["error"] == "model_integrity")
        {
            return Failure("转写模型文件 SHA-256 校验失败，请修复本机安装");
        }
        if (!data || !(*data)["text"].isString() || !(*data)["language"].isString() ||
            (*data)["language"].asString() != language ||
            !valid_transcript((*data)["text"].asString()))
        {
            return Failure("转写未得到有效文本，请核对录音和所选转写环境");
        }
        return speech::Transcript{std::string(language), (*data)["text"].asString()};
    }
}

export namespace meowlive::adapters::speech
{
    std::expected<meowlive::adapters::speech::Transcript, std::string>
    transcribe_reference(const meowlive::adapters::speech::VoiceComponents& components,
                         const std::filesystem::path& audio, std::string_view language,
                         std::stop_token stop = {},
                         std::chrono::seconds timeout = std::chrono::seconds(300))
    {
        if (stop.stop_requested())
        {
            return process_error(speech::ProcessError::Cancelled);
        }
        if (!supported_language(language))
        {
            return Failure("转写语言无效");
        }
        if (!speech::valid_whisper_model(components.whisper_model))
        {
            return Failure("Windows Whisper 模型文件不完整");
        }
        const std::array<std::wstring, 9> args{L"-I",
                                               L"-B",
                                               L"-X",
                                               L"utf8",
                                               L"-c",
                                               transcribe_python,
                                               components.whisper_model.native(),
                                               audio.native(),
                                               std::wstring(language.begin(), language.end())};
        return transcribe_audio(components.whisper_python, args, audio, language, stop, timeout);
    }

    std::expected<meowlive::adapters::speech::Transcript, std::string>
    transcribe_with_model(const meowlive::application::InstalledVoiceModel& model,
                          const std::filesystem::path& script, const std::filesystem::path& audio,
                          std::string_view language, std::stop_token stop)
    {
        const auto wide = [](std::string_view value)
        {
            return std::wstring(value.begin(), value.end());
        };
        const std::array<std::wstring, 15> args{L"-I",
                                                L"-B",
                                                L"-X",
                                                L"utf8",
                                                script.native(),
                                                wide(model.engine_id),
                                                model.directory.native(),
                                                L"--languages",
                                                wide(language),
                                                L"--audio",
                                                audio.native(),
                                                L"--model-id",
                                                wide(model.selection.id),
                                                L"--revision",
                                                wide(model.selection.revision)};
        return transcribe_audio(model.interpreter, args, audio, language, stop,
                                std::chrono::seconds(300));
    }
}
#endif
