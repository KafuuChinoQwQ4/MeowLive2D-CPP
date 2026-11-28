module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <json/json.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.windows_voice_models;

import meowlive.standard;
import meowlive.application.voice_models;
#if defined(_WIN32)
import meowlive.adapters.speech.windows_process;
import meowlive.adapters.storage.atomic_file;
import meowlive.protocol.json;
#endif

#if defined(_WIN32)
namespace
{
    using State = meowlive::application::VoiceDownloadState;

    bool token(std::string_view value, std::size_t limit = 64)
    {
        return !value.empty() && value.size() <= limit &&
               std::ranges::all_of(value,
                                   [](char c)
                                   {
                                       return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                              c == '-';
                                   });
    }

    std::optional<Json::Value> read_object(const std::filesystem::path& file, std::size_t limit)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error || size == 0 || size > limit)
        {
            return std::nullopt;
        }
        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream stream(file, std::ios::binary);
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(size)))
        {
            return std::nullopt;
        }
        return meowlive::protocol::json::parse_object(bytes);
    }

    std::string string(const Json::Value& value)
    {
        return value.isString() ? value.asString() : std::string{};
    }

    std::uint64_t size(const Json::Value& value)
    {
        return value.isUInt64() ? value.asUInt64() : 0;
    }

    bool regular_file(const std::filesystem::path& path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error));
    }

    bool matches_size(const std::filesystem::path& path, std::uint64_t bytes)
    {
        std::error_code error;
        return regular_file(path) && std::filesystem::file_size(path, error) == bytes && !error;
    }

    bool safe_relative_path(std::string_view text)
    {
        const std::filesystem::path path(text);
        return !path.empty() && path.is_relative() && text.find_first_of("\\:") == text.npos &&
               std::ranges::all_of(path,
                                   [](const auto& part)
                                   {
                                       return !part.empty() && part != "." && part != "..";
                                   });
    }

    bool valid_files(const Json::Value& files, bool wheels)
    {
        constexpr auto maximum_file_size = 16ULL * 1024 * 1024 * 1024;
        if (!files.isArray() || files.empty() || files.size() > 256)
        {
            return false;
        }
        return std::ranges::all_of(
            files,
            [wheels, maximum_file_size](const auto& file)
            {
                const auto name = string(file["path"]);
                const auto digest = string(file["sha256"]);
                const auto bytes = size(file["bytes"]);
                return safe_relative_path(name) &&
                       (!wheels || (name.ends_with(".whl") && name.find('/') == name.npos)) &&
                       digest.size() == 64 &&
                       digest.find_first_not_of("0123456789abcdef") == digest.npos &&
                       string(file["url"]).starts_with("https://") && bytes &&
                       bytes <= maximum_file_size;
            });
    }

    std::optional<meowlive::application::VoiceDownloadProgress>
    read_progress(const Json::Value& value)
    {
        const auto name = string(value["state"]);
        constexpr std::array states{State::Queued,     State::Downloading, State::Verifying,
                                    State::Installing, State::Ready,       State::Cancelled,
                                    State::Failed};
        const auto state = std::ranges::find_if(
            states,
            [&name](auto candidate)
            {
                return meowlive::application::voice_download_state_name(candidate) == name;
            });
        if (state == states.end())
        {
            return std::nullopt;
        }
        return meowlive::application::VoiceDownloadProgress{
            *state, size(value["received_bytes"]), size(value["total_bytes"]),
            string(value["file"]), string(value["message"])};
    }

    struct PythonCommand
    {
        std::filesystem::path executable;
        std::vector<std::wstring> arguments;
    };

    PythonCommand python_command(const std::filesystem::path& voice_directory)
    {
        if (const auto* override = ::_wgetenv(L"MEOWLIVE_VOICE_PYTHON"); override && *override)
        {
            const std::filesystem::path executable(override);
            return executable.is_absolute() && regular_file(executable)
                       ? PythonCommand{executable, {}}
                       : PythonCommand{};
        }
        const auto local_python = voice_directory / "python311" / "python.exe";
        if (regular_file(local_python))
        {
            return {local_python, {}};
        }
        if (const auto* local = ::_wgetenv(L"LOCALAPPDATA"); local && *local)
        {
            const auto programs = std::filesystem::path(local) / "Programs" / "Python";
            const auto executable = programs / "Python311" / "python.exe";
            if (regular_file(executable))
            {
                return {executable, {}};
            }
            const auto launcher = programs / "Launcher" / "py.exe";
            if (regular_file(launcher))
            {
                return {launcher, {L"-3.11-64"}};
            }
        }
        std::array<wchar_t, 32768> path;
        const auto length = ::GetWindowsDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (length && length < path.size())
        {
            const auto launcher =
                std::filesystem::path(std::wstring_view(path.data(), length)) / "py.exe";
            if (regular_file(launcher))
            {
                return {launcher, {L"-3.11-64"}};
            }
        }
        return {};
    }

    class WindowsVoiceModelStore final : public meowlive::application::VoiceModelStore
    {
    public:
        explicit WindowsVoiceModelStore(const std::filesystem::path& executable_directory)
            : root_(executable_directory / "voice")
        {
            auto catalog = read_object(root_ / "voice-models.json", 512 * 1024);
            if (!catalog || (*catalog)["schema"] != 1 || !(*catalog)["models"].isArray() ||
                !(*catalog)["runtimes"].isObject())
            {
                return;
            }
            catalog_.revision = string((*catalog)["revision"]);
            for (const auto& entry : (*catalog)["models"])
            {
                const auto selection = meowlive::application::VoiceModelSelection{
                    string(entry["id"]), string(entry["revision"])};
                const auto runtime_id = string(entry["runtime_id"]);
                const auto& runtime = (*catalog)["runtimes"][runtime_id];
                const auto runtime_revision = string(runtime["revision"]);
                const auto purpose = string(entry["purpose"]);
                const auto engine = string(entry["engine_id"]);
                const auto version = string(entry["model_version"]);
                if (!token(selection.id) || !token(selection.revision) || !token(runtime_id) ||
                    !token(runtime_revision) || !valid_files(entry["files"], false) ||
                    !valid_files(runtime["wheels"], true) || size(runtime["install_bytes"]) == 0 ||
                    (!runtime["assets"].isNull() && !valid_files(runtime["assets"], false)) ||
                    (!runtime["sdists"].isNull() && !valid_files(runtime["sdists"], false)) ||
                    !entry["languages"].isArray() || entries_.contains(selection.id) ||
                    (purpose != "transcription" &&
                     (purpose != "training" || engine != "gpt-sovits" || version.empty())))
                {
                    continue;
                }
                std::vector<std::string> languages;
                for (const auto& value : entry["languages"])
                {
                    languages.push_back(string(value));
                }
                catalog_.models.push_back({selection, string(entry["name"]), purpose, engine,
                                           string(entry["adapter_revision"]), runtime_id,
                                           runtime_revision, string(entry["requirements"]),
                                           string(entry["license"]), string(entry["source"]),
                                           std::move(languages), version});
                entries_.emplace(selection.id, entry["files"]);
                runtimes_.try_emplace(runtime_id, runtime);
            }
        }

        const meowlive::application::VoiceModelCatalog& catalog() const override
        {
            return catalog_;
        }

        meowlive::application::VoiceModelInstallation
        inspect(const meowlive::application::VoiceModel& model) const override
        {
            meowlive::application::VoiceModelInstallation result;
            const auto& runtime = runtimes_.at(model.runtime_id);
            const auto& files = entries_.at(model.selection.id);
            const auto runtime_bytes = size(runtime["install_bytes"]);
            result.installed_bytes = runtime_bytes;
            for (const auto& file : files)
            {
                result.installed_bytes += size(file["bytes"]);
            }
            result.installed = model_complete(model);
            const bool runtime_ready = installed_runtime(model);
            const bool scripts_present = model_scripts_present(model);
            result.ready = result.installed && runtime_ready && scripts_present;
            if (result.ready)
            {
                return result;
            }
            if (!runtime_ready)
            {
                add_missing(result, runtime["wheels"]);
                add_missing(result, runtime["sdists"]);
                add_missing(result, runtime["assets"]);
            }
            add_missing(result, files);
            result.required_free_bytes = result.download_bytes + result.installed_bytes -
                                         (runtime_ready ? runtime_bytes : 0);

            std::error_code error;
            if (result.installed)
            {
                result.reason = "模型文件已安装，运行环境或应用脚本不可用";
            }
            else if (std::filesystem::exists(model_path(model), error) || error)
            {
                result.reason = "本机模型收据或文件损坏，请修复模型目录";
            }
            else if (!regular_file(root_ / "install-voice-model.py") || !scripts_present)
            {
                result.reason = "应用安装缺少语音组件脚本";
            }
            else
            {
                result.downloadable = true;
                result.reason = "尚未安装";
                const auto space = std::filesystem::space(root_.parent_path(), error);
                if (!error && space.available < result.required_free_bytes)
                {
                    result.downloadable = false;
                    result.reason = "应用所在磁盘的可用空间不足";
                }
            }
            return result;
        }

        std::optional<meowlive::application::InstalledVoiceModel>
        installed(const meowlive::application::VoiceModel& model) const override
        {
            if (!model_complete(model) || !installed_runtime(model) ||
                !model_scripts_present(model))
            {
                return std::nullopt;
            }
            const auto runtime = runtime_path(model);
            return meowlive::application::InstalledVoiceModel{
                model.selection,   model.engine_id, runtime / "Scripts" / "python.exe",
                model_path(model), runtime,         model.model_version};
        }

        std::expected<void, std::string> install(const meowlive::application::VoiceModel& model,
                                                 const Progress& progress,
                                                 std::stop_token stop) override
        {
            auto python = python_command(root_);
            if (python.executable.empty())
            {
                return std::unexpected("请安装 Windows x64 CPython 3.11 后重新检测");
            }
            auto& args = python.arguments;
            args.insert(
                args.end(),
                {L"-I", L"-B", L"-X", L"utf8", (root_ / "install-voice-model.py").native(),
                 std::wstring(model.selection.id.begin(), model.selection.id.end()),
                 std::wstring(model.selection.revision.begin(), model.selection.revision.end()),
                 L"--data-dir", root_.parent_path().native()});
            std::string pending;
            std::optional<meowlive::application::VoiceDownloadProgress> terminal;
            const auto output = meowlive::adapters::speech::run_windows_process(
                python.executable, args, root_, std::chrono::hours(6), 1024 * 1024, stop,
                [&](std::string_view bytes)
                {
                    pending.append(bytes);
                    for (auto end = pending.find('\n'); end != pending.npos;
                         end = pending.find('\n'))
                    {
                        const auto line = meowlive::protocol::json::parse_object(
                            std::string_view(pending).substr(0, end));
                        pending.erase(0, end + 1);
                        if (auto update = line ? read_progress(*line) : std::nullopt)
                        {
                            if (meowlive::application::voice_download_active(update->state))
                            {
                                progress(std::move(*update));
                            }
                            else
                            {
                                terminal = std::move(update);
                            }
                        }
                    }
                });
            if (terminal && terminal->state == State::Ready && output)
            {
                return {};
            }
            if (terminal && terminal->state == State::Failed)
            {
                return std::unexpected(std::move(terminal->message));
            }
            return std::unexpected("无法执行模型安装，请检测 Windows x64 CPython 3.11 和磁盘空间");
        }

        std::optional<meowlive::application::VoiceDownload>
        load_download(const meowlive::application::VoiceModel& model) const override
        {
            const auto record = read_object(task_path(model.selection.id), 8192);
            if (!record || (*record)["revision"] != model.selection.revision)
            {
                return std::nullopt;
            }
            auto id = string((*record)["id"]);
            auto progress = read_progress(*record);
            if (!token(id) || !progress)
            {
                return std::nullopt;
            }
            return meowlive::application::VoiceDownload{std::move(id), model.selection,
                                                        std::move(*progress)};
        }

        bool save_download(const meowlive::application::VoiceDownload& download) override
        {
            const auto path = task_path(download.model.id);
            auto temporary = path;
            temporary += L".tmp";
            Json::Value record(Json::objectValue);
            record["id"] = download.id;
            record["revision"] = download.model.revision;
            record["state"] = std::string(
                meowlive::application::voice_download_state_name(download.progress.state));
            record["received_bytes"] = static_cast<Json::UInt64>(download.progress.received);
            record["total_bytes"] = static_cast<Json::UInt64>(download.progress.total);
            record["file"] = download.progress.file;
            record["message"] = download.progress.message;
            return meowlive::adapters::storage::write_atomic_file(
                path, temporary, meowlive::protocol::json::serialize(record));
        }

    private:
        bool model_complete(const meowlive::application::VoiceModel& model) const
        {
            const auto directory = model_path(model);
            const auto receipt = read_object(directory / "receipt.json", 4096);
            if (!receipt || (*receipt)["schema"] != 1 || (*receipt)["id"] != model.selection.id ||
                (*receipt)["revision"] != model.selection.revision ||
                (*receipt)["runtime_revision"] != model.runtime_revision)
            {
                return false;
            }
            return std::ranges::all_of(entries_.at(model.selection.id),
                                       [&directory](const auto& file)
                                       {
                                           return matches_size(directory / string(file["path"]),
                                                               size(file["bytes"]));
                                       });
        }

        std::filesystem::path model_path(const meowlive::application::VoiceModel& model) const
        {
            return root_ / "models" / model.selection.id / model.selection.revision;
        }

        std::filesystem::path runtime_path(const meowlive::application::VoiceModel& model) const
        {
            return root_ / "runtimes" / model.runtime_id / model.runtime_revision;
        }

        std::filesystem::path task_path(const std::string& model) const
        {
            return root_ / "downloads" / (model + ".json");
        }

        bool installed_runtime(const meowlive::application::VoiceModel& model) const
        {
            const auto directory = runtime_path(model);
            const auto receipt = read_object(directory / "receipt.json", 4096);
            const auto flow_model = model.model_version == "v3" || model.model_version == "v4";
            return receipt && (*receipt)["schema"] == 1 &&
                   (*receipt)["runtime_revision"] == model.runtime_revision &&
                   regular_file(directory / "Scripts" / "python.exe") &&
                   (model.purpose != "training" ||
                    (regular_file(directory / "engine" / "GPT_SoVITS" / "s1_train.py") &&
                     regular_file(directory / "engine" / "GPT_SoVITS" /
                                  (flow_model ? "s2_train_v3_lora.py" : "s2_train.py")) &&
                     regular_file(directory / "engine" / "GPT_SoVITS" / "text" / "G2PWModel" /
                                  "g2pW.onnx") &&
                     regular_file(directory / "bin" / "ffmpeg.exe")));
        }

        bool model_scripts_present(const meowlive::application::VoiceModel& model) const
        {
            if (model.purpose == "training")
            {
                return regular_file(root_ / "train-gpt-sovits.py") &&
                       regular_file(root_ / "gpt_training_runtime.py") &&
                       regular_file(root_ / "engine_workspace.py");
            }
            return regular_file(root_ / "voice_recognition.py");
        }

        void add_missing(meowlive::application::VoiceModelInstallation& result,
                         const Json::Value& files) const
        {
            for (const auto& file : files)
            {
                const auto name = string(file["path"]);
                const auto bytes = size(file["bytes"]);
                if (!matches_size(root_ / ".staging" / "files" / string(file["sha256"]) /
                                      std::filesystem::path(name).filename(),
                                  bytes))
                {
                    result.download_bytes += bytes;
                    result.missing.push_back({name, bytes});
                }
            }
        }

        const std::filesystem::path root_;
        meowlive::application::VoiceModelCatalog catalog_;
        std::map<std::string, Json::Value> entries_;
        std::map<std::string, Json::Value> runtimes_;
    };
}
#endif

export namespace meowlive::adapters::speech
{
    std::unique_ptr<meowlive::application::VoiceModelStore>
    windows_voice_model_store(const std::filesystem::path& executable_directory)
    {
#if defined(_WIN32)
        return std::make_unique<WindowsVoiceModelStore>(executable_directory);
#else
        (void)executable_directory;
        return {};
#endif
    }
}
