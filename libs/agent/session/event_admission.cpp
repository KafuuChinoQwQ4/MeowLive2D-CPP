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
    meowlive::agent::Submission
    AgentSession::submit(std::vector<meowlive::domain::LiveEvent> events, std::uint64_t now,
                         std::uint64_t wall, std::optional<std::uint64_t> deadline, bool persisted)
    {
        expire(now);
        meowlive::agent::Submission result;
        auto pending =
            std::ranges::count_if(records_,
                                  [](const auto& row)
                                  {
                                      return row.status == meowlive::agent::EventStatus::Pending;
                                  });
        for (auto& event : events)
        {
            if (seen_.contains(event.id) || std::ranges::any_of(records_,
                                                                [&event](const auto& row)
                                                                {
                                                                    return active_row(row) &&
                                                                           row.event.id == event.id;
                                                                }))
            {
                ++result.duplicates;
                continue;
            }
            const auto lifetime =
                deadline ? (*deadline > now ? *deadline - now : 0)
                         : meowlive::domain::response_lifetime_ms(event, wall, limits_.event_ttl_ms,
                                                                  limits_.room_enter_ttl_ms);
            if (!event.occurred_at_ms)
            {
                event.occurred_at_ms = wall;
            }
            if (lifetime && static_cast<std::size_t>(pending) >= limits_.pending_capacity)
            {
                if (!make_room(event))
                {
                    ++result.unscheduled;
                    continue;
                }
                --pending;
            }
            std::erase_if(records_,
                          [&event](const auto& row)
                          {
                              return row.event.id == event.id;
                          });
            interaction_.observe(event, now);
            seen_.insert(event.id);
            seen_order_.push_back(event.id);
            if (seen_order_.size() > limits_.dedup_capacity)
            {
                seen_.erase(seen_order_.front());
                seen_order_.pop_front();
            }
            records_.push_back({std::move(event),
                                lifetime ? meowlive::agent::EventStatus::Pending
                                         : meowlive::agent::EventStatus::Expired,
                                now + lifetime,
                                {},
                                {},
                                0u,
                                persisted});
            pending += static_cast<bool>(lifetime);
            ++result.accepted;
        }
        prune();
        return result;
    }

    void AgentSession::expire(std::uint64_t now)
    {
        for (auto& row : records_)
        {
            if (row.status == meowlive::agent::EventStatus::Pending && row.deadline <= now)
            {
                row.status = meowlive::agent::EventStatus::Expired;
            }
        }
        prune();
    }

    bool AgentSession::active_row(const meowlive::agent::EventRecord& row)
    {
        return row.status == meowlive::agent::EventStatus::Pending ||
               row.status == meowlive::agent::EventStatus::Deciding ||
               row.status == meowlive::agent::EventStatus::Queued ||
               row.status == meowlive::agent::EventStatus::Synthesizing ||
               row.status == meowlive::agent::EventStatus::Ready ||
               row.status == meowlive::agent::EventStatus::Playing;
    }

    unsigned AgentSession::admission_priority(const meowlive::domain::LiveEvent& event)
    {
        if (std::holds_alternative<meowlive::domain::SuperChat>(event.kind))
        {
            return 3;
        }
        if (std::holds_alternative<meowlive::domain::Gift>(event.kind))
        {
            return 2;
        }
        return std::holds_alternative<meowlive::domain::Chat>(event.kind) ? 1 : 0;
    }

    bool AgentSession::make_room(const meowlive::domain::LiveEvent& incoming)
    {
        const auto priority = admission_priority(incoming);
        meowlive::agent::EventRecord* displaced{};
        for (auto& row : records_)
        {
            if (row.status == meowlive::agent::EventStatus::Pending &&
                admission_priority(row.event) < priority &&
                (!displaced || std::pair{admission_priority(row.event), row.event.occurred_at_ms} <
                                   std::pair{admission_priority(displaced->event),
                                             displaced->event.occurred_at_ms}))
            {
                displaced = &row;
            }
        }
        if (!displaced)
        {
            return false;
        }
        displaced->status = meowlive::agent::EventStatus::Skipped;
        displaced->error = "队列已满，为更高优先级互动让出位置";
        if (narrowed_ == displaced->event.id)
        {
            narrowed_.reset();
        }
        return true;
    }

    void AgentSession::prune()
    {
        if (records_.size() <= limits_.history_limit)
        {
            return;
        }
        auto terminal = std::ranges::count_if(records_,
                                              [](const auto& row)
                                              {
                                                  return !active_row(row);
                                              });
        if (static_cast<std::size_t>(terminal) <= limits_.history_limit)
        {
            return;
        }
        terminal -= static_cast<std::ptrdiff_t>(limits_.history_limit);
        std::erase_if(records_,
                      [&terminal](const auto& row)
                      {
                          if (terminal && !active_row(row))
                          {
                              --terminal;
                              return true;
                          }
                          return false;
                      });
    }
}
