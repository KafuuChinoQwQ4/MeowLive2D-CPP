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
    std::optional<meowlive::agent::DecisionWork> AgentSession::begin(std::uint64_t now)
    {
        expire(now);
        if (paused_ || flight_ || playback_ || now < cooldown_until_)
        {
            return {};
        }
        auto candidates = select(now);
        if (candidates.empty() && (!settings_.proactive_enabled || now < proactive_after_))
        {
            return {};
        }
        meowlive::agent::DecisionWork work{++next_id_,
                                           {settings_.persona,
                                            settings_.system_prompt,
                                            settings_.topic,
                                            {},
                                            {history_.begin(), history_.end()},
                                            {}}};
        Flight flight{.id = work.id, .preferred = std::move(candidates.preferred)};
        for (const auto& group : candidates.groups)
        {
            std::vector<std::string> ids;
            for (auto* row : group)
            {
                row->status = meowlive::agent::EventStatus::Deciding;
                work.request.events.push_back(row->event);
                if (row->persisted && row->event.viewer_identity)
                {
                    work.memory_event_ids.push_back(row->event.id);
                }
                ids.push_back(row->event.id);
            }
            flight.groups.push_back(std::move(ids));
        }
        if (work.request.events.size() == 1)
        {
            const auto& event = work.request.events.front();
            flight.required =
                std::holds_alternative<meowlive::domain::SuperChat>(event.kind) ||
                std::holds_alternative<meowlive::domain::RoomEnter>(event.kind) ||
                (candidates.read_all && std::holds_alternative<meowlive::domain::Chat>(event.kind));
        }
        flight_ = std::move(flight);
        last_error_.clear();
        return work;
    }

    void AgentSession::fail(std::uint64_t id, std::string message, std::uint64_t now)
    {
        if (flight_ && flight_->id == id)
        {
            release(*flight_, meowlive::agent::EventStatus::Failed, message);
            flight_.reset();
            last_error_ = std::move(message);
            cooldown(now);
            prune();
        }
    }

    void AgentSession::release(const Flight& flight, meowlive::agent::EventStatus status,
                               const std::string& error)
    {
        for (auto* row : flight_rows(flight))
        {
            row->status = status;
            row->error = error;
        }
    }

    void AgentSession::cooldown(std::uint64_t now)
    {
        cooldown_until_ = now + settings_.cooldown_ms;
        proactive_after_ = cooldown_until_;
    }
}
