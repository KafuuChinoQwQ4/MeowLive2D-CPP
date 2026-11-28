module;

#include <json/json.h>

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
import meowlive.protocol.json;

namespace meowlive::server
{
    bool TrainingJobs::activate(std::string_view id)
    {
        const std::lock_guard lock(mutex_);
        const auto job = this->find(id);
        if (updating_ || !active_.empty() || !auditioning_.empty() || job == jobs_.end() ||
            job->state != State::Completed || !this->verified_weights(*job, {}))
        {
            return false;
        }
        auto proposed = active_versions_;
        proposed[job->voice_id] = job->id;
        Json::Value object(Json::objectValue);
        for (const auto& [voice, version] : proposed)
        {
            object[voice] = version;
        }
        auto temporary = selections_;
        temporary += ".tmp";
        if (!meowlive::adapters::storage::write_atomic_file(selections_, temporary,
                                                            json::serialize(object)))
        {
            return false;
        }
        active_versions_ = std::move(proposed);
        return true;
    }

    std::expected<std::optional<meowlive::adapters::ModelWeights>, std::string>
    TrainingJobs::weights_for_voice(std::string_view voice, std::stop_token stop)
    {
        const std::lock_guard lock(mutex_);
        const auto selected = active_versions_.find(voice);
        if (selected == active_versions_.end())
        {
            return std::nullopt;
        }
        const auto job = this->find(selected->second);
        if (job != jobs_.end())
        {
            if (auto weights = this->verified_weights(*job, stop))
            {
                return std::move(weights);
            }
        }
        return std::unexpected(std::string("已启用的训练权重摘要无效"));
    }

    void TrainingJobs::with_audition_weights(
        std::string_view id, std::stop_token stop,
        const std::function<void(const std::string&, const meowlive::adapters::ModelWeights&)>&
            audition)
    {
        std::unique_lock lock(mutex_);
        const auto job = this->find(id);
        if (closed_ || updating_ || !active_.empty() || !auditioning_.empty() ||
            job == jobs_.end() || job->state != State::Completed || stop.stop_requested())
        {
            return;
        }
        const auto saved = *job;
        auditioning_ = saved.id;
        lock.unlock();

        // Keep this version available until both verification and synthesis finish.
        const struct ReleaseAudition
        {
            meowlive::server::TrainingJobs& jobs;
            ~ReleaseAudition()
            {
                const std::lock_guard lock(jobs.mutex_);
                jobs.auditioning_.clear();
            }
        } release{*this};
        if (const auto weights = this->verified_weights(saved, stop);
            weights && !stop.stop_requested())
        {
            audition(saved.voice_id, *weights);
        }
    }

    void TrainingJobs::load_selections()
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(selections_, error);
        if (error || !size || size > 32 * 1024)
        {
            return;
        }
        std::ifstream stream(selections_, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return;
        }
        if (const auto object = json::parse_object(bytes))
        {
            for (const auto& voice : object->getMemberNames())
            {
                const auto& selection = (*object)[voice];
                if (selection.isString())
                {
                    const auto job = this->find(selection.asString());
                    if (job != jobs_.end() && job->voice_id == voice &&
                        job->state == State::Completed)
                    {
                        active_versions_.emplace(voice, job->id);
                    }
                }
            }
        }
    }

    std::optional<meowlive::adapters::ModelWeights>
    TrainingJobs::verified_weights(const app::TrainingJob& job, std::stop_token stop) const
    {
#if defined(_WIN32)
        const auto path = directory_ / job.id;
        if (meowlive::adapters::speech::verify_training_artifacts(path, job.id, job.model_version,
                                                                  stop))
        {
            return meowlive::adapters::ModelWeights{path / "artifacts" / "gpt.ckpt",
                                                    path / "artifacts" / "sovits.pth"};
        }
#else
        (void)job;
        (void)stop;
#endif
        return std::nullopt;
    }

    bool TrainingJobs::artifact_files(std::string_view id) const
    {
        const auto path = directory_ / std::string(id) / "artifacts";
        std::error_code error;
        return std::filesystem::is_regular_file(path / "gpt.ckpt", error) &&
               std::filesystem::is_regular_file(path / "sovits.pth", error);
    }
}
