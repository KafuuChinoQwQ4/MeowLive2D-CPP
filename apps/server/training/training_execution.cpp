module;

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.training_jobs;

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
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;

namespace meowlive::server
{
    void TrainingJobs::progress(std::string_view id, std::string_view phase, unsigned percent)
    {
        const std::lock_guard lock(mutex_);
        const auto job = this->find(id);
        if (job != jobs_.end() && app::apply_training_progress(*job, phase, percent, utc_now()))
        {
            if (!this->persist(*job))
            {
                LOG_ERROR << "training_progress_write_failed id=" << job->id;
            }
        }
    }

    void TrainingJobs::run(const std::string& id, meowlive::application::SpeechWorker& speech,
                           const std::optional<app::InstalledVoiceModel>& model,
                           std::stop_token stop) noexcept
    try
    {
        std::string error = "训练任务失败";
        auto state = State::Failed;
        try
        {
#if defined(_WIN32)
            std::string version;
            {
                const std::lock_guard lock(mutex_);
                const auto job = this->find(id);
                if (job == jobs_.end())
                {
                    throw std::logic_error("active_training_job_missing");
                }
                version = job->model_version;
            }
            const auto trainer =
                model ? meowlive::adapters::speech::Trainer{model->interpreter,
                                                            model->runtime_directory / "engine",
                                                            runner_, model->directory}
                      : trainer_;
            const bool admitted = speech.edit_voices(
                [&]() -> std::optional<app::SpeechVoices>
                {
                    const auto outcome = meowlive::adapters::speech::run_training(
                        trainer, directory_ / id, id, version, stop,
                        [&](std::string_view phase, unsigned percent)
                        {
                            this->progress(id, phase, percent);
                        });
                    if (outcome)
                    {
                        state = State::Completed;
                        error.clear();
                    }
                    else
                    {
                        error = outcome.error();
                    }
                    return std::nullopt;
                });
            if (!admitted)
            {
                error = "播报进行中或资源正在更新";
            }
#else
            (void)speech;
            (void)model;
#endif
            if (state != State::Completed && stop.stop_requested())
            {
                state = State::Cancelled;
                error = "已取消训练";
            }
        }
        catch (...)
        {
            const auto kind = meowlive::platform::log_current_exception(
                "training.execute", meowlive::platform::ExceptionDetails::Omit);
            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
            state = State::Failed;
            error = "训练任务发生内部错误，请查看服务端日志";
        }
        std::unique_lock lock(mutex_);
        const auto job = this->find(id);
        if (job == jobs_.end())
        {
            throw std::logic_error("active_training_job_missing");
        }
        job->state = state;
        if (state == State::Completed)
        {
            job->progress = 100;
        }
        job->message = std::move(error);
        job->updated_at_ms = utc_now();
        const auto outcome = *job;
        terminal_save_pending_ = true;
        lock.unlock();
        bool reported = false;
        bool final_attempt = false;
        while (!this->persist_outcome(outcome))
        {
            if (!reported)
            {
                LOG_ERROR << "training_status_write_failed id=" << id;
                reported = true;
            }
            if (final_attempt)
            {
                return;
            }
            lock.lock();
            final_attempt = terminal_save_ready_.wait_for(lock, std::chrono::seconds(2),
                                                          [this]
                                                          {
                                                              return closed_;
                                                          });
            lock.unlock();
        }
        lock.lock();
        terminal_save_pending_ = false;
        active_.clear();
    }
    catch (...)
    {
        meowlive::platform::log_current_exception("training.finish",
                                                  meowlive::platform::ExceptionDetails::Omit);
        std::terminate();
    }
}
