module;

#include <meowlive/msvc_stl.hpp>

module meowlive.server.training_worker;

import meowlive.standard;
#if defined(_WIN32)
import meowlive.adapters.speech.training;
import meowlive.adapters.speech.voice_components;
#endif
import meowlive.adapters.speech.windows_voice_models;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;

namespace meowlive::server
{
    class TrainingWorker::Impl
    {
    public:
        Impl(const std::filesystem::path& data, const std::filesystem::path& executable_directory,
             const std::string& speech_url)
#if defined(_WIN32)
            : components_(
                  meowlive::adapters::speech::voice_component_paths(executable_directory, data)),
              work_directory_(data / "training" / "transcribe"),
              recognizer_(executable_directory / "voice" / "voice_recognition.py"),
#else
            :
#endif
              models_(meowlive::adapters::speech::windows_voice_model_store(executable_directory)),
              jobs_(data, executable_directory, models_)
        {
#if defined(_WIN32)
            if (const auto* python = ::_wgetenv(L"MEOWLIVE_WHISPER_PYTHON"); python && *python)
            {
                components_.whisper_python = python;
            }
            if (const auto* model = ::_wgetenv(L"MEOWLIVE_WHISPER_MODEL"); model && *model)
            {
                components_.whisper_model = model;
            }
            const auto* gpt = ::_wgetenv(L"MEOWLIVE_MANAGED_DEFAULT_GPT_WEIGHTS");
            const auto* sovits = ::_wgetenv(L"MEOWLIVE_MANAGED_DEFAULT_SOVITS_WEIGHTS");
            if (gpt && *gpt && sovits && *sovits)
            {
                auto managed = std::make_shared<meowlive::adapters::ManagedModelWeights>(
                    speech_url, meowlive::adapters::ModelWeights{gpt, sovits});
                if (managed->configured())
                {
                    weights_ = std::move(managed);
                }
            }
#else
            (void)speech_url;
#endif
        }

        bool configured() const
        {
#if defined(_WIN32)
            std::error_code error;
            return components_.whisper_python.is_absolute() &&
                   std::filesystem::is_regular_file(components_.whisper_python, error) &&
                   meowlive::adapters::speech::valid_whisper_model(components_.whisper_model);
#else
            return false;
#endif
        }

        bool transcribe(std::string request_id, std::string audio, std::string language,
                        std::optional<meowlive::application::InstalledVoiceModel> model,
                        meowlive::server::TrainingWorker::Reply reply)
        {
            const std::lock_guard lock(mutex_);
            if (shutdown_.stop_requested())
            {
                return false;
            }
            if (!queue_)
            {
                queue_ = std::make_unique<meowlive::application::WorkQueue>(1);
            }
            return queue_->try_submit(
                       [this, request_id = std::move(request_id), audio = std::move(audio),
                        language = std::move(language), model = std::move(model),
                        reply = std::move(reply)]() noexcept
                       {
                           auto result = this->perform(request_id, audio, language, model);
                           try
                           {
                               reply(std::move(result));
                           }
                           catch (...)
                           {
                               const auto kind = meowlive::platform::log_current_exception(
                                   "training.transcribe.reply",
                                   meowlive::platform::ExceptionDetails::Omit);
                               if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
                               {
                                   std::terminate();
                               }
                           }
                       }) == meowlive::application::WorkSubmission::Accepted;
        }

        void close()
        {
            shutdown_.request_stop();
            models_.close();
            const std::lock_guard lock(mutex_);
            queue_.reset();
            jobs_.close();
        }

        meowlive::application::VoiceModelLibrary& voice_models() noexcept
        {
            return models_;
        }

        meowlive::server::TrainingJobs& jobs() noexcept
        {
            return jobs_;
        }

        std::shared_ptr<meowlive::adapters::ManagedModelWeights> model_control() const
        {
            return weights_;
        }

    private:
        meowlive::server::TrainingWorker::Result
        perform(const std::string& request_id, std::string_view bytes, const std::string& language,
                const std::optional<meowlive::application::InstalledVoiceModel>& model) noexcept
        try
        {
            using Failure = meowlive::server::TranscriptionFailure;
            using Kind = meowlive::server::TranscriptionFailureKind;
#if defined(_WIN32)
            std::error_code error;
            std::filesystem::create_directories(work_directory_, error);
            const auto path = work_directory_ / request_id;
            if (error || !std::filesystem::create_directory(path, error))
            {
                return std::unexpected(Failure{Kind::OperationFailed, "无法创建转写工作目录"});
            }
            const struct RecordingDirectory
            {
                const std::filesystem::path& path;
                ~RecordingDirectory()
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }
            } cleanup{path};
            const auto filename = path / "reference.wav";
            std::ofstream recording(filename, std::ios::binary);
            recording.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            recording.close();
            if (!recording)
            {
                return std::unexpected(Failure{Kind::OperationFailed, "无法写入参考录音"});
            }
            auto result = model
                              ? meowlive::adapters::speech::transcribe_with_model(
                                    *model, recognizer_, filename, language, shutdown_.get_token())
                              : meowlive::adapters::speech::transcribe_reference(
                                    components_, filename, language, shutdown_.get_token());
            if (result)
            {
                return meowlive::server::Transcription{
                    std::move(result->text), std::move(result->language),
                    model ? std::optional(model->selection) : std::nullopt};
            }
            return std::unexpected(Failure{Kind::OperationFailed, std::move(result.error())});
#else
            (void)request_id;
            (void)bytes;
            (void)language;
            (void)model;
            return std::unexpected(Failure{Kind::OperationFailed, "未配置 Windows Whisper"});
#endif
        }
        catch (...)
        {
            const auto kind = meowlive::platform::log_current_exception(
                "training.transcribe", meowlive::platform::ExceptionDetails::Omit);
            using Failure = meowlive::server::TranscriptionFailure;
            using Kind = meowlive::server::TranscriptionFailureKind;
            return std::unexpected(Failure{kind == meowlive::platform::ExceptionKind::OutOfMemory
                                               ? Kind::OutOfMemory
                                               : Kind::InternalError,
                                           {}});
        }

#if defined(_WIN32)
        meowlive::adapters::speech::VoiceComponents components_;
        std::filesystem::path work_directory_;
        std::filesystem::path recognizer_;
#endif
        std::mutex mutex_;
        std::stop_source shutdown_;
        std::unique_ptr<meowlive::application::WorkQueue> queue_;
        meowlive::application::VoiceModelLibrary models_;
        meowlive::server::TrainingJobs jobs_;
        std::shared_ptr<meowlive::adapters::ManagedModelWeights> weights_;
    };

    TrainingWorker::TrainingWorker(const std::filesystem::path& data,
                                   const std::filesystem::path& executable_directory,
                                   const std::string& speech_url)
        : impl_(std::make_unique<Impl>(data, executable_directory, speech_url))
    {
    }

    TrainingWorker::~TrainingWorker()
    {
        impl_->close();
    }

    bool TrainingWorker::configured() const
    {
        return impl_->configured();
    }

    bool TrainingWorker::transcribe(std::string request_id, std::string audio, std::string language,
                                    std::optional<meowlive::application::InstalledVoiceModel> model,
                                    meowlive::server::TrainingWorker::Reply reply)
    {
        return impl_->transcribe(std::move(request_id), std::move(audio), std::move(language),
                                 std::move(model), std::move(reply));
    }

    void TrainingWorker::close()
    {
        impl_->close();
    }

    meowlive::application::VoiceModelLibrary& TrainingWorker::voice_models() noexcept
    {
        return impl_->voice_models();
    }

    meowlive::server::TrainingJobs& TrainingWorker::jobs() noexcept
    {
        return impl_->jobs();
    }

    std::shared_ptr<meowlive::adapters::ManagedModelWeights> TrainingWorker::model_control() const
    {
        return impl_->model_control();
    }
}
