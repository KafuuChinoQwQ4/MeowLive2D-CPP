module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.training_jobs;

import meowlive.standard;
#if defined(_WIN32)
import meowlive.adapters.speech.native_training;
import meowlive.adapters.speech.voice_components;
#endif
import meowlive.adapters.storage.atomic_file;
import meowlive.adapters.speech.model_weights;
import meowlive.application.speech_worker;
import meowlive.application.training_job;
import meowlive.application.voice_models;
import meowlive.protocol.json;

namespace meowlive::server
{
    namespace app = meowlive::application;
    using State = meowlive::application::TrainingState;
    namespace json = meowlive::protocol::json;

    constexpr std::size_t max_jobs = 128;

    std::uint64_t utc_now()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    bool valid_id(std::string_view value)
    {
        return value.size() == 36 && value[8] == '-' && value[13] == '-' && value[18] == '-' &&
               value[23] == '-' &&
               std::ranges::all_of(
                   value,
                   [](char character)
                   {
                       return std::string_view("0123456789abcdefABCDEF-").contains(character);
                   });
    }

    void write_base_model(Json::Value& target, const app::TrainingJob& job)
    {
        if (job.base_model)
        {
            target["base_model_id"] = job.base_model->id;
            target["base_model_revision"] = job.base_model->revision;
        }
    }

    Json::Value serialize(const app::TrainingJob& job)
    {
        Json::Value properties(Json::objectValue);
        properties["id"] = job.id;
        properties["name"] = job.name;
        properties["voice_id"] = job.voice_id;
        properties["model_version"] = job.model_version;
        properties["status"] = std::string(app::training_state_name(job.state));
        properties["progress"] = job.progress;
        properties["message"] = job.message;
        properties["clip_count"] = job.clip_count;
        properties["created_at_ms"] = Json::UInt64(job.created_at_ms);
        properties["updated_at_ms"] = Json::UInt64(job.updated_at_ms);
        properties["version_id"] =
            job.state == State::Completed ? Json::Value(job.id) : Json::Value{};
        write_base_model(properties, job);
        return properties;
    }

    std::optional<app::TrainingJob> read_job(const std::filesystem::path& directory,
                                             std::string_view filename)
    {
        std::error_code error;
        const auto path = directory / filename;
        const auto size = std::filesystem::file_size(path, error);
        if (error || !size || size > 16 * 1024)
        {
            return std::nullopt;
        }
        std::ifstream stream(path, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), bytes.size()))
        {
            return std::nullopt;
        }
        const auto parsed = json::parse_object(bytes);
        if (!parsed)
        {
            return std::nullopt;
        }
        for (const auto* key : {"id", "name", "voice_id", "model_version", "status", "message"})
        {
            if (!(*parsed)[key].isString())
            {
                return std::nullopt;
            }
        }
        if ((*parsed)["id"].asString() != directory.filename().string() ||
            !json::is_unsigned_32((*parsed)["progress"]) ||
            !json::is_unsigned_32((*parsed)["clip_count"]) ||
            !(*parsed)["created_at_ms"].isUInt64() || !(*parsed)["updated_at_ms"].isUInt64())
        {
            return std::nullopt;
        }
        const auto status = (*parsed)["status"].asString();
        constexpr std::array states{State::Queued,     State::Preparing, State::Training,
                                    State::Validating, State::Completed, State::Cancelled,
                                    State::Failed};
        const auto found = std::ranges::find(states, status, app::training_state_name);
        if (found == states.end())
        {
            return std::nullopt;
        }
        std::optional<app::VoiceModelSelection> base_model;
        if ((*parsed)["base_model_id"].isString() && (*parsed)["base_model_revision"].isString())
        {
            base_model.emplace((*parsed)["base_model_id"].asString(),
                               (*parsed)["base_model_revision"].asString());
        }
        return app::TrainingJob{.id = (*parsed)["id"].asString(),
                                .name = (*parsed)["name"].asString(),
                                .voice_id = (*parsed)["voice_id"].asString(),
                                .model_version = (*parsed)["model_version"].asString(),
                                .state = *found,
                                .progress = (*parsed)["progress"].asUInt(),
                                .clip_count = (*parsed)["clip_count"].asUInt(),
                                .created_at_ms = (*parsed)["created_at_ms"].asUInt64(),
                                .updated_at_ms = (*parsed)["updated_at_ms"].asUInt64(),
                                .message = (*parsed)["message"].asString(),
                                .base_model = std::move(base_model)};
    }

    bool stage(const std::filesystem::path& path, const app::TrainingJob& job,
               const app::TrainingRequest& request)
    {
        std::error_code error;
        if (!std::filesystem::create_directory(path, error) ||
            !std::filesystem::create_directory(path / "clips", error) ||
            !std::filesystem::create_directory(path / "artifacts", error))
        {
            return false;
        }
        Json::Value data(Json::objectValue);
        data["schema"] = 1;
        data["id"] = job.id;
        data["voice_id"] = job.voice_id;
        data["gpt_epochs"] = request.gpt_epochs;
        data["sovits_epochs"] = request.sovits_epochs;
        data["model_version"] = job.model_version;
        write_base_model(data, job);
        data["text_mode"] = "reviewed_text";
        data["fp16"] = true;
        data["performance"]["batch_size"] = 1;
        data["performance"]["data_workers"] = 0;
        data["performance"]["cpu_threads"] = 2;
        data["performance"]["gpu_index"] = 0;
        data["performance"]["low_memory"] = true;
        data["artifacts"]["gpt"] = "artifacts/gpt.ckpt";
        data["artifacts"]["sovits"] = "artifacts/sovits.pth";
        for (std::size_t index = 0; index < request.clips.size(); ++index)
        {
            const auto& clip = request.clips[index];
            const auto number = std::to_string(index);
            const auto filename = "clips/" + std::string(3 - number.size(), '0') + number + ".wav";
            std::ofstream stream(path / filename, std::ios::binary);
            stream.write(clip.wav.data(), static_cast<std::streamsize>(clip.wav.size()));
            stream.close();
            if (!stream)
            {
                return false;
            }
            Json::Value metadata(Json::objectValue);
            metadata["path"] = filename;
            metadata["language"] = clip.language;
            metadata["text"] = clip.text;
            data["clips"].append(std::move(metadata));
        }
        return meowlive::adapters::storage::write_atomic_file(path / "job.json", path / "job.tmp",
                                                              json::serialize(data));
    }
}

export namespace meowlive::server
{
    class TrainingJobs
    {
    public:
        using StartError = std::variant<app::VoiceModelError, std::string>;

        explicit TrainingJobs(const std::filesystem::path& data,
                              const std::filesystem::path& executable_directory,
                              const app::VoiceModelLibrary& models)
            : directory_(data / "training" / "jobs"),
              selections_(directory_.parent_path() / "active-versions.json"), models_(models)
        {
#if defined(_WIN32)
            const auto components =
                meowlive::adapters::speech::voice_component_paths(executable_directory, data);
            runner_ = executable_directory / "voice" / "train-gpt-sovits.py";
            trainer_ = {components.inference_python, components.gpt_sovits, runner_, {}};
            if (const auto* value = ::_wgetenv(L"MEOWLIVE_TRAINING_PYTHON"); value && *value)
            {
                trainer_.python = value;
            }
            if (const auto* value = ::_wgetenv(L"MEOWLIVE_GPT_SOVITS_ROOT"); value && *value)
            {
                trainer_.engine = value;
            }
            if (const auto* value = ::_wgetenv(L"MEOWLIVE_TRAINING_RUNNER"); value && *value)
            {
                trainer_.runner = value;
            }
#else
            (void)executable_directory;
#endif
            std::error_code error;
            for (std::filesystem::directory_iterator files(directory_, error), end;
                 !error && files != end && jobs_.size() < max_jobs; files.increment(error))
            {
                const auto& entry = *files;
                if (entry.is_directory(error) && valid_id(entry.path().filename().string()))
                {
                    auto job = read_job(entry.path(), "outcome.json");
                    if (!job || app::active_training(job->state))
                    {
                        job = read_job(entry.path(), "status.json");
                    }
                    if (!job)
                    {
                        continue;
                    }
                    if (app::active_training(job->state))
                    {
                        job->state = State::Failed;
                        job->message = "主服务在训练运行时退出，未找到已保存的训练结果";
                    }
                    jobs_.push_back(std::move(*job));
                }
            }
            this->load_selections();
        }

        ~TrainingJobs()
        {
            this->close();
        }

        bool configured() const
        {
#if defined(_WIN32)
            return meowlive::adapters::speech::trainer_present(trainer_);
#else
            return false;
#endif
        }

        Json::Value snapshot() const
        {
            const std::lock_guard lock(mutex_);
            Json::Value result(Json::objectValue);
            result["enabled"] = this->configured() || models_.has_training_model();
            result["busy"] = !active_.empty() || !auditioning_.empty();
            result["jobs"] = Json::Value(Json::arrayValue);
            result["versions"] = Json::Value(Json::arrayValue);
            for (const auto& job : jobs_)
            {
                auto status = serialize(job);
                if (terminal_save_pending_ && job.id == active_)
                {
                    status["message"] = "训练结果暂未保存，主服务正在重试";
                    status["save_pending"] = true;
                }
                result["jobs"].append(std::move(status));
                if (job.state == State::Completed)
                {
                    Json::Value version(Json::objectValue);
                    version["id"] = job.id;
                    version["job_id"] = job.id;
                    version["name"] = job.name;
                    version["voice_id"] = job.voice_id;
                    version["model_version"] = job.model_version;
                    write_base_model(version, job);
                    version["engine"] = "gpt-sovits";
                    version["created_at_ms"] = Json::UInt64(job.created_at_ms);
                    version["available"] = artifact_files(job.id);
                    const auto chosen = active_versions_.find(job.voice_id);
                    version["active"] =
                        chosen != active_versions_.end() && chosen->second == job.id;
                    version["auditioned"] = false;
                    version["saved"] = !terminal_save_pending_ || job.id != active_;
                    result["versions"].append(std::move(version));
                }
            }
            return result;
        }

        std::expected<Json::Value, StartError>
        start(std::string id, const meowlive::application::TrainingRequest& request,
              std::shared_ptr<meowlive::application::SpeechWorker> speech)
        {
            std::optional<app::InstalledVoiceModel> model;
            if (request.base_model)
            {
                auto selected =
                    models_.resolve_training(*request.base_model, request.model_version);
                if (!selected)
                {
                    return std::unexpected(selected.error());
                }
                model = std::move(*selected);
            }
            else if (!this->configured())
            {
                return std::unexpected(std::string("未配置 Windows 训练环境"));
            }
            const std::lock_guard lock(mutex_);
            if (closed_ || updating_ || !valid_id(id))
            {
                return std::unexpected(std::string("Windows 训练环境尚未配置"));
            }
            if (!active_.empty())
            {
                return std::unexpected(std::string("已有训练任务正在运行"));
            }
            if (!auditioning_.empty())
            {
                return std::unexpected(std::string("训练版本正在试听"));
            }
            if (jobs_.size() >= max_jobs)
            {
                return std::unexpected(std::string("训练任务数量已达上限"));
            }
            std::error_code error;
            std::filesystem::create_directories(directory_, error);
            const auto path = directory_ / id;
            if (error || std::filesystem::exists(path, error) || error)
            {
                return std::unexpected(std::string("无法创建训练任务目录"));
            }
            const auto now = utc_now();
            const app::TrainingJob job{.id = id,
                                       .name = request.name,
                                       .voice_id = request.voice_id,
                                       .model_version =
                                           model ? model->model_version : request.model_version,
                                       .state = State::Queued,
                                       .clip_count = static_cast<unsigned>(request.clips.size()),
                                       .created_at_ms = now,
                                       .updated_at_ms = now,
                                       .base_model = request.base_model};
            if (!stage(path, job, request) || !this->persist(job))
            {
                std::filesystem::remove_all(path, error);
                return std::unexpected(std::string("无法写入训练清单或素材"));
            }
            jobs_.push_back(job);
            active_ = id;
            worker_ = std::jthread(
                [this, id = std::move(id), model = std::move(model),
                 speech = std::move(speech)](std::stop_token stop) noexcept
                {
                    this->run(id, *speech, model, stop);
                });
            return serialize(job);
        }

        bool cancel(std::string_view id)
        {
            const std::lock_guard lock(mutex_);
            return !id.empty() && active_ == id && !terminal_save_pending_ &&
                   worker_.request_stop();
        }

        bool remove(std::string_view id)
        {
            const std::lock_guard lock(mutex_);
            const auto result = this->find(id);
            if (updating_ || result == jobs_.end() || active_ == id || auditioning_ == id)
            {
                return false;
            }
            const auto selected = active_versions_.find(result->voice_id);
            if (selected != active_versions_.end() && selected->second == id)
            {
                return false;
            }
            std::error_code error;
            const auto job = directory_ / result->id;
            const auto removed = directory_ / (result->id + ".removing");
            std::filesystem::rename(job, removed, error);
            if (error)
            {
                return false;
            }
            jobs_.erase(result);
            std::filesystem::remove_all(removed, error);
            return true;
        }

        bool activate(std::string_view id);

        std::expected<std::optional<meowlive::adapters::ModelWeights>, std::string>
        weights_for_voice(std::string_view voice, std::stop_token stop);

        void with_audition_weights(
            std::string_view id, std::stop_token stop,
            const std::function<void(const std::string&, const meowlive::adapters::ModelWeights&)>&
                audition);

        bool freeze_for_update()
        {
            const std::lock_guard lock(mutex_);
            if (closed_ || updating_ || !active_.empty() || !auditioning_.empty())
            {
                return false;
            }
            updating_ = true;
            return true;
        }

        void resume_after_update_check()
        {
            const std::lock_guard lock(mutex_);
            updating_ = false;
        }

        bool has_unsaved_outcome() const
        {
            const std::lock_guard lock(mutex_);
            return terminal_save_pending_;
        }

        void close()
        {
            std::jthread owned;
            {
                const std::lock_guard lock(mutex_);
                closed_ = true;
                worker_.request_stop();
                terminal_save_ready_.notify_all();
                owned = std::move(worker_);
            }
        }

    private:
        void load_selections();

        std::optional<meowlive::adapters::ModelWeights>
        verified_weights(const app::TrainingJob& job, std::stop_token stop) const;

        std::vector<app::TrainingJob>::iterator find(std::string_view id)
        {
            return std::ranges::find_if(jobs_,
                                        [id](const auto& job)
                                        {
                                            return job.id == id;
                                        });
        }

        bool artifact_files(std::string_view id) const;

        bool persist(const app::TrainingJob& job) const
        {
            const auto path = directory_ / job.id;
            return meowlive::adapters::storage::write_atomic_file(
                path / "status.json", path / "status.tmp", json::serialize(serialize(job)));
        }

        bool persist_outcome(const app::TrainingJob& job) const
        {
            const auto path = directory_ / job.id;
            const auto bytes = json::serialize(serialize(job));
            return meowlive::adapters::storage::write_atomic_file(path / "status.json",
                                                                  path / "status.tmp", bytes) ||
                   meowlive::adapters::storage::write_atomic_file(path / "outcome.json",
                                                                  path / "outcome.tmp", bytes);
        }

        void progress(std::string_view id, std::string_view phase, unsigned percent);

        void run(const std::string& id, meowlive::application::SpeechWorker& speech,
                 const std::optional<app::InstalledVoiceModel>& model,
                 std::stop_token stop) noexcept;

        const std::filesystem::path directory_;
        const std::filesystem::path selections_;
        const app::VoiceModelLibrary& models_;
#if defined(_WIN32)
        std::filesystem::path runner_;
        meowlive::adapters::speech::Trainer trainer_;
#endif
        mutable std::mutex mutex_;
        std::condition_variable terminal_save_ready_;
        std::vector<app::TrainingJob> jobs_;
        std::map<std::string, std::string, std::less<>> active_versions_;
        std::jthread worker_;
        std::string active_;
        std::string auditioning_;
        bool terminal_save_pending_{};
        bool updating_{};
        bool closed_{};
    };
}
