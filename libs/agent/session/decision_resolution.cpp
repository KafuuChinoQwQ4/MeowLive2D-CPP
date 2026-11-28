module;

#include <meowlive/msvc_stl.hpp>

module meowlive.agent.session;

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.application.llm;
import meowlive.domain.live_event;
import meowlive.domain.speech;

namespace meowlive::agent
{
    namespace
    {
        using SelectedReplies = std::unordered_set<std::string>;

        std::expected<SelectedReplies, std::string>
        validate_replies(const meowlive::application::AgentDecision& decision,
                         const std::vector<meowlive::agent::EventRecord*>& rows,
                         const std::vector<std::vector<std::string>>& groups)
        {
            SelectedReplies selected(decision.reply_to.begin(), decision.reply_to.end());
            const auto unknown_reply = [&rows](const auto& event_id)
            {
                return std::ranges::find(rows, event_id,
                                         [](const auto* row) -> const std::string&
                                         {
                                             return row->event.id;
                                         }) == rows.end();
            };
            if (selected.size() != decision.reply_to.size() ||
                std::ranges::any_of(selected, unknown_reply) ||
                (!decision.text && !selected.empty()) ||
                (decision.text && !rows.empty() && selected.empty()))
            {
                return std::unexpected("invalid_reply_to");
            }
            for (const auto& group : groups)
            {
                const auto matches = std::ranges::count_if(group,
                                                           [&selected](const auto& member)
                                                           {
                                                               return selected.contains(member);
                                                           });
                if (matches && static_cast<std::size_t>(matches) != group.size())
                {
                    return std::unexpected("incomplete_gift_group");
                }
            }
            return selected;
        }

        std::expected<meowlive::agent::PreparedSpeech, std::string>
        prepare_speech(const std::vector<meowlive::agent::EventRecord*>& rows,
                       const SelectedReplies& selected, const std::optional<std::string>& text)
        {
            meowlive::agent::PreparedSpeech prepared;
            for (auto* row : rows)
            {
                if (selected.contains(row->event.id))
                {
                    if (!prepared.turn.user.empty())
                    {
                        prepared.turn.user += '\n';
                    }
                    prepared.turn.user += meowlive::agent::describe_event(row->event);
                    prepared.turn.assistant += meowlive::agent::read_prefix(row->event);
                    prepared.events.push_back(row->event);
                    if (row->persisted)
                    {
                        prepared.persisted_events.push_back({row->event.source, row->event.id});
                    }
                }
            }
            if (text)
            {
                auto normalized = meowlive::domain::normalize_speech_text(*text);
                if (!normalized)
                {
                    return std::unexpected("invalid_decision_text");
                }
                for (std::string_view internal : {"好感度", "好感分", "熟悉度", "内部评分",
                                                  "affinity score", "familiarity score"})
                {
                    if (normalized->contains(internal))
                    {
                        return std::unexpected("invalid_decision_text");
                    }
                }
                prepared.turn.assistant += *normalized;
                if (!meowlive::domain::normalize_speech_text(prepared.turn.assistant, 1200))
                {
                    return std::unexpected("invalid_broadcast_length");
                }
            }
            return prepared;
        }
    }

    std::expected<std::optional<meowlive::agent::PreparedSpeech>, std::string>
    AgentSession::resolve(std::uint64_t id, meowlive::application::AgentDecision decision,
                          std::uint64_t now)
    {
        if (!flight_ || flight_->id != id)
        {
            return std::unexpected("stale_decision");
        }
        const auto rows = flight_rows(*flight_);
        const auto expired = [now](const auto* row)
        {
            return row->deadline <= now;
        };
        if (std::ranges::any_of(rows, expired))
        {
            for (auto* row : rows)
            {
                row->status = expired(row) ? meowlive::agent::EventStatus::Expired
                                           : meowlive::agent::EventStatus::Pending;
            }
            flight_.reset();
            prune();
            return std::nullopt;
        }
        const auto pending_interaction = [](const auto& row)
        {
            return row.status == meowlive::agent::EventStatus::Pending &&
                   !std::holds_alternative<meowlive::domain::RoomEnter>(row.event.kind);
        };
        if (rows.size() == 1 &&
            std::holds_alternative<meowlive::domain::RoomEnter>(rows.front()->event.kind) &&
            (std::ranges::any_of(records_, pending_interaction) ||
             interaction_.busy(settings_.interaction, rows.size(), now)))
        {
            release(*flight_, meowlive::agent::EventStatus::Skipped, {});
            flight_.reset();
            return std::nullopt;
        }
        if (flight_->required && decision.reply_to.empty() && !decision.text)
        {
            decision.reply_to.push_back(rows.front()->event.id);
            decision.text =
                std::holds_alternative<meowlive::domain::RoomEnter>(rows.front()->event.kind)
                    ? "很高兴见到你。"
                    : "这条留言我收到了。";
        }
        const auto selected = validate_replies(decision, rows, flight_->groups);
        if (!selected)
        {
            return std::unexpected(selected.error());
        }
        auto prepared = prepare_speech(rows, *selected, decision.text);
        if (!prepared)
        {
            return std::unexpected(prepared.error());
        }
        if (decision.topic)
        {
            if (!decision.topic->empty() &&
                !meowlive::domain::normalize_speech_text(*decision.topic, 200))
            {
                return std::unexpected("invalid_decision_topic");
            }
            settings_.topic = *decision.topic;
        }
        narrowed_.reset();
        for (auto* row : rows)
        {
            if (!selected->contains(row->event.id))
            {
                row->status = row->skips++ < 2 ? meowlive::agent::EventStatus::Pending
                                               : meowlive::agent::EventStatus::Skipped;
                if (row->status == meowlive::agent::EventStatus::Pending &&
                    flight_->preferred == row->event.id)
                {
                    narrowed_ = row->event.id;
                }
            }
        }
        flight_.reset();
        cooldown(now);
        if (!decision.text)
        {
            prune();
            return std::nullopt;
        }
        return std::move(*prepared);
    }
}
