module;

#include <meowlive/msvc_stl.hpp>

module meowlive.agent.session;

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.agent.runtime;
import meowlive.application.llm;
import meowlive.application.reply_completion;
import meowlive.application.scheduler;
import meowlive.domain.live_event;
import meowlive.domain.speech;

namespace meowlive::agent
{
    void AgentSession::enqueued(meowlive::agent::PreparedSpeech prepared,
                                const meowlive::application::SpeechTask& task, std::uint64_t now)
    {
        for (const auto& event : prepared.events)
        {
            if (std::holds_alternative<meowlive::domain::RoomEnter>(event.kind))
            {
                interaction_.welcomed(event, now);
            }
        }
        playback_ = Playback{task.id, task.generation, context_, std::move(prepared)};
        sync(task, now);
    }

    void AgentSession::enqueue_failed(const meowlive::agent::PreparedSpeech& prepared,
                                      std::string message)
    {
        for (const auto& event : prepared.events)
        {
            auto& row = *find(event.id);
            row.status = meowlive::agent::EventStatus::Failed;
            row.error = message;
        }
        last_error_ = std::move(message);
        prune();
    }

    void AgentSession::sync(const meowlive::application::SpeechTask& task, std::uint64_t now)
    {
        if (!playback_ || playback_->id != task.id || playback_->generation != task.generation)
        {
            return;
        }
        for (const auto& event : playback_->prepared.events)
        {
            auto& row = *find(event.id);
            row.status = event_status(task.status);
            row.speech_id = task.id;
            row.error = task.error.value_or("");
        }
        using Status = meowlive::domain::SpeechStatus;
        if (task.status < Status::Completed)
        {
            return;
        }
        if (task.status == Status::Completed && playback_->context == context_)
        {
            interaction_.completed(playback_->prepared.events, playback_->prepared.turn.assistant,
                                   now);
            history_.push_back(std::move(playback_->prepared.turn));
            if (history_.size() > limits_.conversation_limit)
            {
                history_.pop_front();
            }
        }
        if (task.error)
        {
            last_error_ = *task.error;
        }
        playback_.reset();
        cooldown(now);
        prune();
    }

    meowlive::agent::EventStatus AgentSession::event_status(meowlive::domain::SpeechStatus status)
    {
        using Status = meowlive::domain::SpeechStatus;
        switch (status)
        {
        case Status::Queued:
            return meowlive::agent::EventStatus::Queued;
        case Status::Synthesizing:
            return meowlive::agent::EventStatus::Synthesizing;
        case Status::Ready:
            return meowlive::agent::EventStatus::Ready;
        case Status::Playing:
            return meowlive::agent::EventStatus::Playing;
        case Status::Completed:
            return meowlive::agent::EventStatus::Completed;
        case Status::Cancelled:
            return meowlive::agent::EventStatus::Cancelled;
        case Status::Failed:
            return meowlive::agent::EventStatus::Failed;
        case Status::Unknown:
            return meowlive::agent::EventStatus::Unknown;
        }
        return meowlive::agent::EventStatus::Unknown;
    }
}
