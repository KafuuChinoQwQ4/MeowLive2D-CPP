#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

import meowlive.adapters.speech.training;
import meowlive.adapters.speech.voice_components;
import meowlive.adapters.speech.windows_process;

namespace
{
    namespace voice = meowlive::adapters::speech;

    bool process_boundaries()
    {
        std::array<wchar_t, 32'768> path{};
        const auto length =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size())
        {
            return false;
        }
        const std::filesystem::path executable(std::wstring_view(path.data(), length));
        const auto directory = executable.parent_path();
        const std::array<std::wstring, 2> output{L"--print", L"spaces \"quoted\" \\"};
        const auto normal = voice::run_windows_process(executable, output, directory,
                                                       std::chrono::seconds(2), 64, {});
        const auto limited = voice::run_windows_process(executable, output, directory,
                                                        std::chrono::seconds(2), 3, {});
        const std::array<std::wstring, 1> slow{L"--slow"};
        const auto timeout = voice::run_windows_process(executable, slow, directory,
                                                        std::chrono::milliseconds(150), 64, {});
        std::stop_source cancelled;
        cancelled.request_stop();
        const auto stopped = voice::run_windows_process(
            executable, slow, directory, std::chrono::seconds(2), 64, cancelled.get_token());
        return normal && *normal == "spaces \"quoted\" \\" && !limited &&
               limited.error() == voice::ProcessError::Output && !timeout &&
               timeout.error() == voice::ProcessError::Timeout && !stopped &&
               stopped.error() == voice::ProcessError::Cancelled;
    }
}

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--print")
    {
        std::fputs(argv[2], stdout);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--slow")
    {
        ::Sleep(3000);
        return 0;
    }
    if (!process_boundaries())
    {
        std::fputs("native voice process quoting, output limit, timeout or cancellation failed\n",
                   stderr);
        return 1;
    }

    const auto* python = ::_wgetenv(L"MEOWLIVE_ASR_TEST_PYTHON");
    const auto* model = ::_wgetenv(L"MEOWLIVE_ASR_TEST_MODEL");
    const auto* audio = ::_wgetenv(L"MEOWLIVE_ASR_TEST_AUDIO");
    if (!python || !model || !audio)
    {
        return 0;
    }
    auto components = voice::VoiceComponents{.whisper_python = python, .whisper_model = model};
    if (const auto* engine = ::_wgetenv(L"MEOWLIVE_ASR_TEST_GPT_SOVITS"))
    {
        components.gpt_sovits = engine;
        if (const auto* interpreter = ::_wgetenv(L"MEOWLIVE_ASR_TEST_INFERENCE_PYTHON"))
        {
            components.inference_python = interpreter;
        }
    }

    const auto capabilities = voice::inspect_voice_components(components);
    if (!capabilities.whisper_cpu || !capabilities.whisper_model_available)
    {
        std::fputs("Windows Whisper runtime is missing or incompatible\n", stderr);
        return 1;
    }
    const auto result = voice::transcribe_reference(components, audio, "en");
    if (!result || result->language != "en" || result->text.empty())
    {
        std::fprintf(stderr, "%s\n", result ? "language mismatch" : result.error().c_str());
        return 1;
    }
    std::printf("Python %s, faster-whisper %s, Torch %s, GPT-SoVITS CPU %d CUDA %d\n%s\n",
                capabilities.whisper_python_version.c_str(), capabilities.whisper_version.c_str(),
                capabilities.torch_version.c_str(), capabilities.inference_cpu,
                capabilities.inference_gpu, result->text.c_str());
}
