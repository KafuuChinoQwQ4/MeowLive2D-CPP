module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.training_job;

import meowlive.standard;
import meowlive.application.voice_models;

export namespace meowlive::application
{
    enum class TrainingState
    {
        Queued,
        Preparing,
        Training,
        Validating,
        Completed,
        Cancelled,
        Failed,
    };

    struct TrainingClip
    {
        std::string language;
        std::string text;
        std::string wav;
    };

    struct TrainingRequest
    {
        std::string name;
        std::string voice_id;
        std::string model_version{"v2"};
        unsigned gpt_epochs{};
        unsigned sovits_epochs{};
        std::vector<meowlive::application::TrainingClip> clips;
        std::optional<meowlive::application::VoiceModelSelection> base_model;
    };

    struct TrainingJob
    {
        std::string id;
        std::string name;
        std::string voice_id;
        std::string model_version;
        meowlive::application::TrainingState state{};
        unsigned progress{};
        unsigned clip_count{};
        std::uint64_t created_at_ms{};
        std::uint64_t updated_at_ms{};
        std::string message;
        std::optional<meowlive::application::VoiceModelSelection> base_model;
    };

    std::string_view training_state_name(meowlive::application::TrainingState state)
    {
        switch (state)
        {
        case meowlive::application::TrainingState::Queued:
            return "queued";
        case meowlive::application::TrainingState::Preparing:
            return "preparing";
        case meowlive::application::TrainingState::Training:
            return "training";
        case meowlive::application::TrainingState::Validating:
            return "validating";
        case meowlive::application::TrainingState::Completed:
            return "completed";
        case meowlive::application::TrainingState::Cancelled:
            return "cancelled";
        case meowlive::application::TrainingState::Failed:
            return "failed";
        }
        return "failed";
    }

    bool active_training(meowlive::application::TrainingState state)
    {
        return state == meowlive::application::TrainingState::Queued ||
               state == meowlive::application::TrainingState::Preparing ||
               state == meowlive::application::TrainingState::Training ||
               state == meowlive::application::TrainingState::Validating;
    }

    bool apply_training_progress(meowlive::application::TrainingJob& job, std::string_view phase,
                                 unsigned progress, std::uint64_t now)
    {
        auto next = meowlive::application::TrainingState::Failed;
        if (phase == "preparing")
        {
            next = meowlive::application::TrainingState::Preparing;
        }
        else if (phase == "training")
        {
            next = meowlive::application::TrainingState::Training;
        }
        else if (phase == "validating")
        {
            next = meowlive::application::TrainingState::Validating;
        }
        if (!meowlive::application::active_training(job.state) ||
            !meowlive::application::active_training(next) || next < job.state ||
            progress < job.progress || progress > 99)
        {
            return false;
        }
        job.state = next;
        job.progress = progress;
        job.updated_at_ms = now;
        return true;
    }
}
