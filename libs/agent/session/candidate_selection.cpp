module;

#include <meowlive/msvc_stl.hpp>

module meowlive.agent.session;

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.domain.live_event;

namespace meowlive::agent
{
    meowlive::agent::AgentSession::Selection AgentSession::select(std::uint64_t now)
    {
        using Record = meowlive::agent::EventRecord;
        std::vector<Record*> pending;
        bool important{};
        for (auto& row : records_)
        {
            if (row.status == meowlive::agent::EventStatus::Pending)
            {
                pending.push_back(&row);
                important = important ||
                            !std::holds_alternative<meowlive::domain::RoomEnter>(row.event.kind);
            }
        }
        const bool busy = interaction_.busy(settings_.interaction, pending.size(), now);
        Selection result;
        result.read_all =
            settings_.interaction.chat_mode == meowlive::agent::ChatMode::All ||
            (settings_.interaction.chat_mode == meowlive::agent::ChatMode::Auto && !busy);
        std::erase_if(
            pending,
            [&](auto* row)
            {
                if (std::holds_alternative<meowlive::domain::RoomEnter>(row->event.kind) &&
                    (important || busy ||
                     !interaction_.may_welcome(row->event, settings_.interaction, now)))
                {
                    row->status = meowlive::agent::EventStatus::Skipped;
                    return true;
                }
                return false;
            });
        if (pending.empty())
        {
            return {};
        }
        const auto sc = std::ranges::min_element(
            pending, {},
            [](auto* row)
            {
                return std::pair{
                    !std::holds_alternative<meowlive::domain::SuperChat>(row->event.kind),
                    row->event.occurred_at_ms};
            });
        if (std::holds_alternative<meowlive::domain::SuperChat>((*sc)->event.kind))
        {
            result.groups.push_back({*sc});
            return result;
        }
        result.preferred = prioritize_candidates(pending, now);
        group_candidates(pending, result);
        return result;
    }

    std::optional<std::string>
    AgentSession::prioritize_candidates(std::vector<meowlive::agent::EventRecord*>& pending,
                                        std::uint64_t now) const
    {
        const bool ordinary_due = interaction_.non_chat_rounds() >= 2;
        const auto chat = [](const auto* row)
        {
            return std::holds_alternative<meowlive::domain::Chat>(row->event.kind);
        };
        if (std::ranges::any_of(pending,
                                [this](auto* row)
                                {
                                    return !interaction_.saturated(row->event);
                                }))
        {
            std::erase_if(pending,
                          [this](auto* row)
                          {
                              return interaction_.saturated(row->event);
                          });
        }
        else
        {
            const auto oldest = std::ranges::min_element(
                pending, {},
                [&](const auto* row)
                {
                    return std::tuple{!(ordinary_due && chat(row)), row->event.occurred_at_ms,
                                      std::string_view(row->event.id)};
                });
            const auto viewer = meowlive::agent::viewer_key((*oldest)->event);
            std::erase_if(pending,
                          [&viewer](auto* row)
                          {
                              return meowlive::agent::viewer_key(row->event) != viewer;
                          });
        }
        const auto narrowed = [this](const auto* row)
        {
            return narrowed_ == row->event.id;
        };
        if (std::ranges::any_of(pending, narrowed))
        {
            std::erase_if(pending,
                          [&narrowed](const auto* row)
                          {
                              return !narrowed(row);
                          });
        }
        const auto focused = [this, now](const auto* row)
        {
            return interaction_.focused(row->event, now);
        };
        const auto order = [&](auto* row)
        {
            return std::tuple{!(ordinary_due && chat(row)), !focused(row),
                              !std::holds_alternative<meowlive::domain::Gift>(row->event.kind),
                              row->event.occurred_at_ms};
        };
        std::ranges::stable_sort(pending, {}, order);
        const auto target = std::ranges::find_if(pending,
                                                 [&](const auto* row)
                                                 {
                                                     return (ordinary_due && chat(row)) ||
                                                            focused(row) || narrowed(row);
                                                 });
        if (target != pending.end())
        {
            return (*target)->event.id;
        }
        return std::nullopt;
    }

    void AgentSession::group_candidates(std::vector<meowlive::agent::EventRecord*>& pending,
                                        Selection& result) const
    {
        const auto& first_kind = pending.front()->event.kind;
        if (std::holds_alternative<meowlive::domain::RoomEnter>(first_kind) ||
            (result.read_all && std::holds_alternative<meowlive::domain::Chat>(first_kind)))
        {
            result.groups.push_back({pending.front()});
            return;
        }
        if (result.read_all)
        {
            std::erase_if(pending,
                          [](auto* row)
                          {
                              return !std::holds_alternative<meowlive::domain::Gift>(
                                  row->event.kind);
                          });
        }
        std::size_t included{};
        std::size_t prefix_characters{};
        while (!pending.empty() && included < limits_.batch_size)
        {
            auto* first = pending.front();
            pending.erase(pending.begin());
            const auto prefix = meowlive::agent::read_prefix(first->event);
            const auto count = std::ranges::count_if(prefix,
                                                     [](unsigned char byte)
                                                     {
                                                         return (byte & 0xc0) != 0x80;
                                                     });
            if (prefix_characters + count > 700)
            {
                continue;
            }
            prefix_characters += count;
            result.groups.push_back({first});
            ++included;
            std::erase_if(pending,
                          [&](auto* row)
                          {
                              if (included < limits_.batch_size &&
                                  meowlive::agent::same_gift(first->event, row->event))
                              {
                                  result.groups.back().push_back(row);
                                  ++included;
                                  return true;
                              }
                              return false;
                          });
        }
    }
}
