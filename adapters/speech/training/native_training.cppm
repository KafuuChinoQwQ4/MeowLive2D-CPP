module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <json/json.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#endif

export module meowlive.adapters.speech.native_training;

#if defined(_WIN32)
import meowlive.adapters.speech.windows_process;
import meowlive.protocol.json;

export namespace meowlive::adapters::speech
{
    struct Trainer
    {
        std::filesystem::path python;
        std::filesystem::path engine;
        std::filesystem::path runner;
        std::filesystem::path models;
    };

    struct ArtifactPair
    {
        std::string gpt_sha256;
        std::string sovits_sha256;
    };

    using TrainingProgress = std::function<void(std::string_view, unsigned)>;
}

namespace
{
    namespace speech = meowlive::adapters::speech;
    namespace json = meowlive::protocol::json;

    struct Sha256
    {
        ~Sha256()
        {
            if (hash)
            {
                ::BCryptDestroyHash(hash);
            }
            if (algorithm)
            {
                ::BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        }

        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
    };

    std::string digest(const std::filesystem::path& path, std::stop_token stop)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)))
        {
            return {};
        }
        const auto size = std::filesystem::file_size(path, error);
        if (!size || error || size > 2ULL * 1024 * 1024 * 1024)
        {
            return {};
        }
        std::ifstream stream(path, std::ios::binary);
        Sha256 crypto;
        if (!stream ||
            ::BCryptOpenAlgorithmProvider(&crypto.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) <
                0 ||
            ::BCryptCreateHash(crypto.algorithm, &crypto.hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            return {};
        }

        std::array<unsigned char, 65536> chunk{};
        std::uint64_t count{};
        while (!stop.stop_requested() &&
               stream.read(reinterpret_cast<char*>(chunk.data()), chunk.size()).gcount() > 0)
        {
            const auto length = static_cast<ULONG>(stream.gcount());
            count += length;
            if (count > size || ::BCryptHashData(crypto.hash, chunk.data(), length, 0) < 0)
            {
                return {};
            }
        }
        std::array<unsigned char, 32> bytes{};
        if (!stream.eof() || count != size || stop.stop_requested() ||
            ::BCryptFinishHash(crypto.hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
        {
            return {};
        }
        constexpr std::string_view hex = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto byte : bytes)
        {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0xf]);
        }
        return result;
    }

    void report_progress(std::string& pending, std::string_view bytes,
                         const speech::TrainingProgress& report)
    {
        pending.append(bytes);
        std::size_t newline{};
        while ((newline = pending.find('\n')) != std::string::npos)
        {
            const auto message = json::parse_object(std::string_view(pending).substr(0, newline));
            if (message && (*message)["state"].isString() &&
                json::is_unsigned_32((*message)["progress"]) &&
                (*message)["progress"].asUInt() <= 100)
            {
                const auto state = (*message)["state"].asString();
                if (state == "preparing" || state == "training" || state == "validating")
                {
                    report(state, (*message)["progress"].asUInt());
                }
            }
            pending.erase(0, newline + 1);
        }
    }
}

export namespace meowlive::adapters::speech
{
    bool trainer_present(const meowlive::adapters::speech::Trainer& trainer)
    {
        const auto regular = [](const std::filesystem::path& path)
        {
            std::error_code error;
            return path.is_absolute() && std::filesystem::is_regular_file(path, error);
        };
        return regular(trainer.python) && regular(trainer.runner) &&
               regular(trainer.engine / "GPT_SoVITS" / "s1_train.py") &&
               regular(trainer.engine / "GPT_SoVITS" / "s2_train.py");
    }

    std::expected<meowlive::adapters::speech::ArtifactPair, std::string>
    verify_training_artifacts(const std::filesystem::path& directory, std::string_view id,
                              std::string_view version, std::stop_token stop = {})
    {
        const auto invalid = std::unexpected(std::string("成对训练权重或摘要无效"));
        const auto manifest = directory / "artifacts" / "manifest.json";
        std::error_code error;
        const auto size = std::filesystem::file_size(manifest, error);
        if (error || size > 8192 || !size)
        {
            return invalid;
        }
        std::ifstream stream(manifest, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return invalid;
        }
        const auto metadata = json::parse_object(bytes);
        if (!metadata || (*metadata)["job_id"] != Json::Value(std::string(id)) ||
            (*metadata)["model_version"] != Json::Value(std::string(version)) ||
            !(*metadata)["sha256"].isObject())
        {
            return invalid;
        }
        auto gpt = digest(directory / "artifacts" / "gpt.ckpt", stop);
        auto sovits = digest(directory / "artifacts" / "sovits.pth", stop);
        const auto& hashes = (*metadata)["sha256"];
        if (gpt.empty() || sovits.empty() || hashes["gpt"] != Json::Value(gpt) ||
            hashes["sovits"] != Json::Value(sovits))
        {
            return invalid;
        }
        return speech::ArtifactPair{std::move(gpt), std::move(sovits)};
    }

    std::expected<meowlive::adapters::speech::ArtifactPair, std::string>
    run_training(const meowlive::adapters::speech::Trainer& trainer,
                 const std::filesystem::path& directory, std::string_view id,
                 std::string_view version, std::stop_token stop,
                 const meowlive::adapters::speech::TrainingProgress& report)
    {
        std::vector<std::wstring> args{L"-B",
                                       L"-s",
                                       L"-X",
                                       L"utf8",
                                       L"-u",
                                       trainer.runner.native(),
                                       L"--job",
                                       (directory / "job.json").native(),
                                       L"--engine-root",
                                       trainer.engine.native()};
        if (!trainer.models.empty())
        {
            args.insert(args.end(), {L"--model-root", trainer.models.native()});
        }
        std::string pending;
        const auto status = speech::run_windows_process(trainer.python, args, directory,
                                                        std::chrono::hours(2), 256 * 1024, stop,
                                                        [&](std::string_view bytes)
                                                        {
                                                            report_progress(pending, bytes, report);
                                                        });
        if (!status)
        {
            return std::unexpected(
                std::string(stop.stop_requested() ? "训练已取消" : "训练进程失败，请检查任务日志"));
        }
        return speech::verify_training_artifacts(directory, id, version, stop);
    }
}
#endif
