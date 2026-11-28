module;

#if defined(_WIN32)
#include <json/json.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <system_error>
#endif

export module meowlive.adapters.speech.voice_components;

#if defined(_WIN32)
import meowlive.adapters.speech.windows_process;
import meowlive.protocol.json;

export namespace meowlive::adapters::speech
{
    struct VoiceComponents
    {
        std::filesystem::path inference_python;
        std::filesystem::path gpt_sovits;
        std::filesystem::path whisper_python;
        std::filesystem::path whisper_model;
    };

    struct VoiceCapabilities
    {
        std::string inference_python_version;
        std::string whisper_python_version;
        std::string whisper_version;
        std::string torch_version;
        bool whisper_model_available{};
        bool whisper_cpu{};
        bool inference_cpu{};
        bool inference_gpu{};
        bool training_dependencies{};
    };
}

namespace
{
    namespace speech = meowlive::adapters::speech;

    constexpr auto detect_python = LR"PY(
import json
import platform
import struct
import sys
from contextlib import redirect_stdout
result = {"version":platform.python_version(),"win64":sys.platform=="win32" and struct.calcsize("P")==8}
if sys.argv[1] == "whisper":
    try:
        with redirect_stdout(sys.stderr):
            import ctranslate2
            import faster_whisper
            result["whisper"] = faster_whisper.__version__
            result["whisper_cpu"] = "int8" in ctranslate2.get_supported_compute_types("cpu")
    except (ImportError, OSError, RuntimeError):
        pass
else:
    try:
        with redirect_stdout(sys.stderr):
            import torch
            import torchaudio
            import fastapi
            import uvicorn
            result["torch"] = torch.__version__
            result["inference_cpu"] = True
            result["inference_gpu"] = torch.cuda.is_available()
            if result["inference_gpu"]:
                import pytorch_lightning
                result["training_dependencies"] = True
    except (ImportError, OSError, RuntimeError):
        pass
print(json.dumps(result))
)PY";

    bool regular_file(const std::filesystem::path& path)
    {
        std::error_code error;
        return path.is_absolute() && std::filesystem::is_regular_file(path, error) &&
               std::filesystem::file_size(path, error) > 0 && !error;
    }

    Json::Value detect_runtime(const std::filesystem::path& python, const wchar_t* mode,
                               std::stop_token stop)
    {
        if (!regular_file(python) || python.extension() != L".exe")
        {
            return {};
        }
        const std::array<std::wstring, 5> args{L"-I", L"-B", L"-c", detect_python, mode};
        const auto result = speech::run_windows_process(python, args, python.parent_path(),
                                                        std::chrono::seconds(20), 8192, stop);
        if (result)
        {
            const auto data = meowlive::protocol::json::parse_object(*result);
            if (data && (*data)["win64"] == true)
            {
                return *data;
            }
        }
        return {};
    }

    std::string text(const Json::Value& value)
    {
        return value.isString() ? value.asString() : std::string{};
    }
}

export namespace meowlive::adapters::speech
{
    bool valid_whisper_model(const std::filesystem::path& path)
    {
        return regular_file(path / "model.bin") && regular_file(path / "config.json") &&
               regular_file(path / "tokenizer.json") &&
               (regular_file(path / "vocabulary.json") || regular_file(path / "vocabulary.txt")) &&
               regular_file(path / "preprocessor_config.json");
    }

    meowlive::adapters::speech::VoiceComponents
    voice_component_paths(const std::filesystem::path& executable_directory,
                          const std::filesystem::path& data)
    {
        const auto components_in = [](const std::filesystem::path& directory)
        {
            const auto root = directory / "voice";
            return meowlive::adapters::speech::VoiceComponents{
                .inference_python = root / "gpt-sovits-python" / "Scripts" / "python.exe",
                .gpt_sovits = root / "GPT-SoVITS",
                .whisper_python = root / "whisper-python" / "Scripts" / "python.exe",
                .whisper_model = root / "models" / "faster-whisper-large-v3-turbo"};
        };
        auto installed = components_in(executable_directory);
        const auto previous = components_in(data);
        if ((!regular_file(installed.inference_python) ||
             !regular_file(installed.gpt_sovits / "api_v2.py")) &&
            regular_file(previous.inference_python))
        {
            installed.inference_python = previous.inference_python;
            installed.gpt_sovits = previous.gpt_sovits;
        }
        if ((!regular_file(installed.whisper_python) ||
             !meowlive::adapters::speech::valid_whisper_model(installed.whisper_model)) &&
            regular_file(previous.whisper_python))
        {
            installed.whisper_python = previous.whisper_python;
            installed.whisper_model = previous.whisper_model;
        }
        return installed;
    }

    meowlive::adapters::speech::VoiceCapabilities
    inspect_voice_components(const meowlive::adapters::speech::VoiceComponents& paths,
                             std::stop_token stop = {})
    {
        const auto inference = detect_runtime(paths.inference_python, L"gpt-sovits", stop);
        const auto whisper = detect_runtime(paths.whisper_python, L"whisper", stop);
        const auto engine = regular_file(paths.gpt_sovits / "api_v2.py");
        const auto train = regular_file(paths.gpt_sovits / "GPT_SoVITS" / "s1_train.py") &&
                           regular_file(paths.gpt_sovits / "GPT_SoVITS" / "s2_train.py");
        return {.inference_python_version = text(inference["version"]),
                .whisper_python_version = text(whisper["version"]),
                .whisper_version = text(whisper["whisper"]),
                .torch_version = text(inference["torch"]),
                .whisper_model_available = speech::valid_whisper_model(paths.whisper_model),
                .whisper_cpu = whisper["whisper_cpu"] == true,
                .inference_cpu = engine && inference["inference_cpu"] == true,
                .inference_gpu = engine && inference["inference_gpu"] == true,
                .training_dependencies = train && inference["training_dependencies"] == true};
    }
}
#endif
