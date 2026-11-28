module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.voice_models;

import meowlive.standard;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::application
{
    struct VoiceModelSelection
    {
        std::string id;
        std::string revision;
    };

    struct VoiceModel
    {
        meowlive::application::VoiceModelSelection selection;
        std::string name;
        std::string purpose;
        std::string engine_id;
        std::string adapter_revision;
        std::string runtime_id;
        std::string runtime_revision;
        std::string requirements;
        std::string license;
        std::string source;
        std::vector<std::string> languages;
        std::string model_version;
    };

    struct VoiceModelCatalog
    {
        std::string revision;
        std::vector<meowlive::application::VoiceModel> models;
    };

    struct VoiceAssetSize
    {
        std::string name;
        std::uint64_t bytes;
    };

    struct VoiceModelInstallation
    {
        std::vector<meowlive::application::VoiceAssetSize> missing;
        std::uint64_t download_bytes{};
        std::uint64_t installed_bytes{};
        std::uint64_t required_free_bytes{};
        std::string reason;
        bool installed{};
        bool ready{};
        bool downloadable{};
    };

    struct InstalledVoiceModel
    {
        meowlive::application::VoiceModelSelection selection;
        std::string engine_id;
        std::filesystem::path interpreter;
        std::filesystem::path directory;
        std::filesystem::path runtime_directory;
        std::string model_version;
    };

    enum class VoiceDownloadState
    {
        Queued,
        Downloading,
        Verifying,
        Installing,
        Ready,
        Cancelled,
        Failed,
    };

    struct VoiceDownloadProgress
    {
        meowlive::application::VoiceDownloadState state{};
        std::uint64_t received{};
        std::uint64_t total{};
        std::string file;
        std::string message;
    };

    struct VoiceDownload
    {
        std::string id;
        meowlive::application::VoiceModelSelection model;
        meowlive::application::VoiceDownloadProgress progress;
    };

    bool voice_download_active(meowlive::application::VoiceDownloadState state)
    {
        using State = meowlive::application::VoiceDownloadState;
        return state == State::Queued || state == State::Downloading || state == State::Verifying ||
               state == State::Installing;
    }

    std::string_view voice_download_state_name(meowlive::application::VoiceDownloadState state)
    {
        using State = meowlive::application::VoiceDownloadState;
        switch (state)
        {
        case State::Queued:
            return "queued";
        case State::Downloading:
            return "downloading";
        case State::Verifying:
            return "verifying";
        case State::Installing:
            return "installing";
        case State::Ready:
            return "ready";
        case State::Cancelled:
            return "cancelled";
        case State::Failed:
            return "failed";
        }
        std::unreachable();
    }

    class VoiceModelStore
    {
    public:
        using Progress = std::function<void(meowlive::application::VoiceDownloadProgress)>;

        virtual ~VoiceModelStore() = default;
        virtual const meowlive::application::VoiceModelCatalog& catalog() const = 0;
        virtual meowlive::application::VoiceModelInstallation
        inspect(const meowlive::application::VoiceModel& model) const = 0;
        virtual std::optional<meowlive::application::InstalledVoiceModel>
        installed(const meowlive::application::VoiceModel& model) const = 0;
        virtual std::expected<void, std::string>
        install(const meowlive::application::VoiceModel& model, const Progress& progress,
                std::stop_token stop) = 0;
        virtual std::optional<meowlive::application::VoiceDownload>
        load_download(const meowlive::application::VoiceModel& model) const = 0;
        virtual bool save_download(const meowlive::application::VoiceDownload& download) = 0;
    };

    enum class VoiceModelError
    {
        UnknownModel,
        RevisionUnavailable,
        WrongPurpose,
        TrainingVersionMismatch,
        UnsupportedLanguage,
        NotReady,
        InstallUnavailable,
        AlreadyInstalled,
        UnknownDownload,
        DownloadFinished,
        Busy,
        Closing,
        Storage,
    };

    class VoiceModelLibrary
    {
    public:
        explicit VoiceModelLibrary(std::unique_ptr<meowlive::application::VoiceModelStore> store)
            : store_(std::move(store))
        {
            if (!store_)
            {
                return;
            }
            for (const auto& model : store_->catalog().models)
            {
                if (auto previous = store_->load_download(model))
                {
                    if (meowlive::application::voice_download_active(previous->progress.state))
                    {
                        if (store_->installed(model))
                        {
                            previous->progress.state = State::Ready;
                        }
                        else
                        {
                            previous->progress.state = State::Failed;
                            previous->progress.message = "上次安装中断，可重试并继续下载";
                        }
                    }
                    tasks_.emplace(model.selection.id, Task{std::move(*previous), {}});
                }
            }
        }

        ~VoiceModelLibrary()
        {
            close();
        }

        const meowlive::application::VoiceModelCatalog& catalog() const
        {
            return store_ ? store_->catalog() : empty_;
        }

        meowlive::application::VoiceModelInstallation
        inspect(const meowlive::application::VoiceModel& model) const
        {
            return store_->inspect(model);
        }

        std::expected<meowlive::application::InstalledVoiceModel,
                      meowlive::application::VoiceModelError>
        resolve_transcription(const meowlive::application::VoiceModelSelection& selection,
                              std::string_view language) const
        {
            const auto model = find_model(selection);
            if (!model)
            {
                return std::unexpected(model.error());
            }
            if ((*model)->purpose != "transcription")
            {
                return std::unexpected(Error::WrongPurpose);
            }
            if (!std::ranges::contains((*model)->languages, language))
            {
                return std::unexpected(Error::UnsupportedLanguage);
            }
            return require_installed(**model);
        }

        std::expected<meowlive::application::InstalledVoiceModel,
                      meowlive::application::VoiceModelError>
        resolve_training(const meowlive::application::VoiceModelSelection& selection,
                         std::string_view version) const
        {
            const auto model = find_model(selection);
            if (!model)
            {
                return std::unexpected(model.error());
            }
            if ((*model)->purpose != "training")
            {
                return std::unexpected(Error::WrongPurpose);
            }
            if (!version.empty() && version != (*model)->model_version)
            {
                return std::unexpected(Error::TrainingVersionMismatch);
            }
            return require_installed(**model);
        }

        bool has_training_model() const
        {
            return std::ranges::any_of(catalog().models,
                                       [this](const auto& model)
                                       {
                                           return model.purpose == "training" &&
                                                  store_->installed(model).has_value();
                                       });
        }

        std::expected<std::string, meowlive::application::VoiceModelError>
        start_download(const meowlive::application::VoiceModelSelection& selection, std::string id)
        {
            const auto model = find_model(selection);
            if (!model)
            {
                return std::unexpected(model.error());
            }
            const std::lock_guard lock(mutex_);
            if (closing_)
            {
                return std::unexpected(Error::Closing);
            }
            const auto previous = tasks_.find(selection.id);
            if (previous != tasks_.end() && meowlive::application::voice_download_active(
                                                previous->second.download.progress.state))
            {
                return previous->second.download.id;
            }
            const auto status = store_->inspect(**model);
            if (status.ready)
            {
                if (previous != tasks_.end() &&
                    previous->second.download.progress.state == State::Ready)
                {
                    return previous->second.download.id;
                }
                return std::unexpected(Error::AlreadyInstalled);
            }
            if (!status.downloadable)
            {
                return std::unexpected(Error::InstallUnavailable);
            }
            if (!queue_)
            {
                queue_ = std::make_unique<meowlive::application::WorkQueue>(2);
            }
            Task task{{id, selection, {State::Queued, 0, status.download_bytes, {}, {}}}, {}};
            if (!store_->save_download(task.download))
            {
                return std::unexpected(Error::Storage);
            }
            const auto stop = task.cancel.get_token();
            auto inserted = tasks_.insert_or_assign(selection.id, std::move(task)).first;
            const auto result = queue_->try_submit(
                [this, model = *model, stop]() noexcept
                {
                    run(*model, stop);
                });
            if (result != meowlive::application::WorkSubmission::Accepted)
            {
                auto& download = inserted->second.download;
                download.progress = {State::Failed, 0, 0, {}, "模型下载队列已满"};
                store_->save_download(download);
                return std::unexpected(Error::Busy);
            }
            return id;
        }

        std::expected<void, meowlive::application::VoiceModelError> cancel(std::string_view id)
        {
            const std::lock_guard lock(mutex_);
            for (auto& [model, task] : tasks_)
            {
                if (task.download.id == id)
                {
                    if (!meowlive::application::voice_download_active(task.download.progress.state))
                    {
                        return std::unexpected(Error::DownloadFinished);
                    }
                    task.cancel.request_stop();
                    return {};
                }
            }
            return std::unexpected(Error::UnknownDownload);
        }

        std::vector<meowlive::application::VoiceDownload> downloads() const
        {
            const std::lock_guard lock(mutex_);
            std::vector<meowlive::application::VoiceDownload> result;
            result.reserve(tasks_.size());
            for (const auto& [model, task] : tasks_)
            {
                result.push_back(task.download);
            }
            return result;
        }

        void close()
        {
            std::unique_ptr<meowlive::application::WorkQueue> queue;
            {
                const std::lock_guard lock(mutex_);
                closing_ = true;
                for (auto& [model, task] : tasks_)
                {
                    task.cancel.request_stop();
                }
                queue = std::move(queue_);
            }
            if (queue)
            {
                queue->drain();
            }
        }

    private:
        using Error = meowlive::application::VoiceModelError;
        using State = meowlive::application::VoiceDownloadState;

        struct Task
        {
            meowlive::application::VoiceDownload download;
            std::stop_source cancel;
        };

        std::expected<const meowlive::application::VoiceModel*, Error>
        find_model(const meowlive::application::VoiceModelSelection& selection) const
        {
            const auto& entries = catalog().models;
            const auto model = std::ranges::find_if(entries,
                                                    [&selection](const auto& entry)
                                                    {
                                                        return entry.selection.id == selection.id;
                                                    });
            if (model == entries.end())
            {
                return std::unexpected(Error::UnknownModel);
            }
            if (model->selection.revision != selection.revision)
            {
                return std::unexpected(Error::RevisionUnavailable);
            }
            return &*model;
        }

        std::expected<meowlive::application::InstalledVoiceModel, Error>
        require_installed(const meowlive::application::VoiceModel& model) const
        {
            auto installed = store_->installed(model);
            if (!installed)
            {
                return std::unexpected(Error::NotReady);
            }
            return std::move(*installed);
        }

        void run(const meowlive::application::VoiceModel& model, std::stop_token stop) noexcept
        try
        {
            auto result = stop.stop_requested()
                              ? std::expected<void, std::string>{std::unexpected(std::string{})}
                              : store_->install(
                                    model,
                                    [this, &model](auto progress)
                                    {
                                        const std::lock_guard lock(mutex_);
                                        tasks_.at(model.selection.id).download.progress =
                                            std::move(progress);
                                    },
                                    stop);
            if (store_->installed(model))
            {
                finish(model.selection.id, State::Ready, {});
            }
            else if (stop.stop_requested())
            {
                finish(model.selection.id, State::Cancelled, "已取消，可重试并继续下载");
            }
            else
            {
                finish(model.selection.id, State::Failed,
                       result ? "安装没有发布有效收据" : std::move(result.error()));
            }
        }
        catch (...)
        {
            if (meowlive::platform::log_current_exception(
                    "voice_models.install", meowlive::platform::ExceptionDetails::Omit) ==
                meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
            finish(model.selection.id, State::Failed, "模型安装异常，请重试");
        }

        void finish(const std::string& key, State state, std::string message)
        {
            const std::lock_guard lock(mutex_);
            auto& download = tasks_.at(key).download;
            download.progress.state = state;
            download.progress.message = std::move(message);
            if (state == State::Ready)
            {
                download.progress.received = download.progress.total;
                download.progress.file.clear();
            }
            if (!store_->save_download(download))
            {
                download.progress.message += "（任务状态保存失败）";
            }
        }

        std::unique_ptr<meowlive::application::VoiceModelStore> store_;
        const meowlive::application::VoiceModelCatalog empty_;
        mutable std::mutex mutex_;
        std::map<std::string, Task, std::less<>> tasks_;
        std::unique_ptr<meowlive::application::WorkQueue> queue_;
        bool closing_{};
    };
}
