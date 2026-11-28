module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.agent.interaction;

import meowlive.standard;

import meowlive.domain.live_event;

namespace
{
    struct CodepointRange
    {
        std::uint32_t first;
        std::uint32_t last;
    };

    constexpr CodepointRange alphanumeric_ranges[]{
#include "unicode_alphanumeric.inc"
    };

    bool alphanumeric(std::uint32_t codepoint)
    {
        const auto range =
            std::ranges::lower_bound(alphanumeric_ranges, codepoint, {}, &CodepointRange::last);
        return range != std::end(alphanumeric_ranges) && codepoint >= range->first;
    }

    bool related_to_question(std::string_view question, std::string_view reply)
    {
        std::string_view previous;
        while (!question.empty())
        {
            const auto first = static_cast<unsigned char>(question.front());
            const auto width = first < 0x80 ? 1u : first < 0xe0 ? 2u : first < 0xf0 ? 3u : 4u;
            if (width > question.size())
            {
                return false;
            }
            auto codepoint =
                static_cast<std::uint32_t>(width == 1 ? first : first & (0x7fu >> width));
            for (unsigned index = 1; index < width; ++index)
            {
                codepoint =
                    (codepoint << 6) | (static_cast<unsigned char>(question[index]) & 0x3fu);
            }
            const auto character = question.substr(0, width);
            question.remove_prefix(width);
            if (alphanumeric(codepoint))
            {
                if (!previous.empty() &&
                    reply.contains(std::string(previous) + std::string(character)))
                {
                    return true;
                }
                previous = character;
            }
        }
        return false;
    }
}

export namespace meowlive::agent
{
    enum class ChatMode
    {
        Auto,
        All,
        Selective
    };

    struct InteractionSettings
    {
        meowlive::agent::ChatMode chat_mode{meowlive::agent::ChatMode::Auto};
        bool welcome_enabled{true};
        unsigned busy_chat_count{6};
        unsigned busy_enter_count{3};
        unsigned busy_pending_count{4};
        std::uint64_t welcome_cooldown_ms{30'000};
        std::uint64_t welcome_viewer_cooldown_ms{600'000};
    };

    std::string viewer_key(const meowlive::domain::LiveEvent& event)
    {
        if (const auto& identity = event.viewer_identity)
        {
            return std::format("{}:{}{}:{}:{}:{}", event.source.size(), event.source,
                               identity->identity_namespace.size(), identity->identity_namespace,
                               static_cast<unsigned>(identity->kind), identity->external_id);
        }
        return std::format("{}:{}:anonymous:{}", event.source.size(), event.source, event.viewer);
    }

    std::string read_prefix(const meowlive::domain::LiveEvent& event)
    {
        std::string text;
        if (const auto* chat = std::get_if<meowlive::domain::Chat>(&event.kind))
        {
            text = chat->text;
        }
        else if (const auto* sc = std::get_if<meowlive::domain::SuperChat>(&event.kind))
        {
            text = std::format("感谢{}的{}元SC。{}", event.viewer, sc->amount_cny, sc->text);
        }
        else if (std::holds_alternative<meowlive::domain::RoomEnter>(event.kind))
        {
            return std::format("欢迎{}，感谢你来到直播间！", event.viewer);
        }
        if (!text.empty() && !text.ends_with("。") && !text.ends_with("！") &&
            !text.ends_with("？") && !text.ends_with("…") &&
            text.find_last_of(".!?") != text.size() - 1)
        {
            text += "。";
        }
        return text;
    }

    std::string describe_event(const meowlive::domain::LiveEvent& event)
    {
        if (const auto* gift = std::get_if<meowlive::domain::Gift>(&event.kind))
        {
            return std::format("{} 赠送 {} × {}", event.viewer, gift->name, gift->count);
        }
        return event.viewer + "：" + meowlive::agent::read_prefix(event);
    }

    bool same_gift(const meowlive::domain::LiveEvent& first,
                   const meowlive::domain::LiveEvent& other)
    {
        const auto* a = std::get_if<meowlive::domain::Gift>(&first.kind);
        const auto* b = std::get_if<meowlive::domain::Gift>(&other.kind);
        const auto age = first.occurred_at_ms > other.occurred_at_ms
                             ? first.occurred_at_ms - other.occurred_at_ms
                             : other.occurred_at_ms - first.occurred_at_ms;
        return a && b && first.viewer_identity && other.viewer_identity &&
               meowlive::agent::viewer_key(first) == meowlive::agent::viewer_key(other) &&
               a->name == b->name && age <= 3000;
    }

    class Interaction
    {
    public:
        void observe(const meowlive::domain::LiveEvent& event, std::uint64_t now)
        {
            auto* times = std::holds_alternative<meowlive::domain::Chat>(event.kind) ? &chats_
                          : std::holds_alternative<meowlive::domain::RoomEnter>(event.kind)
                              ? &entries_
                              : nullptr;
            if (times)
            {
                times->push_back(now);
                if (times->size() > 1000)
                {
                    times->pop_front();
                }
            }
        }

        bool busy(const meowlive::agent::InteractionSettings& settings, std::size_t pending,
                  std::uint64_t now)
        {
            for (auto* times : {&chats_, &entries_})
            {
                while (!times->empty() && now - times->front() >= 60'000)
                {
                    times->pop_front();
                }
            }
            return chats_.size() >= settings.busy_chat_count ||
                   entries_.size() >= settings.busy_enter_count ||
                   pending >= settings.busy_pending_count;
        }

        bool may_welcome(const meowlive::domain::LiveEvent& event,
                         const meowlive::agent::InteractionSettings& settings, std::uint64_t now)
        {
            const auto viewer = welcomes_.find(meowlive::agent::viewer_key(event));
            return settings.welcome_enabled &&
                   (!last_welcome_ || now - *last_welcome_ >= settings.welcome_cooldown_ms) &&
                   (viewer == welcomes_.end() ||
                    now - viewer->second >= settings.welcome_viewer_cooldown_ms);
        }

        void welcomed(const meowlive::domain::LiveEvent& event, std::uint64_t now)
        {
            std::erase_if(welcomes_,
                          [now](const auto& entry)
                          {
                              return now - entry.second >= 86'400'000;
                          });
            if (welcomes_.size() == 4096)
            {
                welcomes_.erase(std::ranges::min_element(welcomes_, {},
                                                         [](const auto& entry)
                                                         {
                                                             return entry.second;
                                                         }));
            }
            last_welcome_ = now;
            welcomes_.insert_or_assign(meowlive::agent::viewer_key(event), now);
        }

        bool saturated(const meowlive::domain::LiveEvent& event) const
        {
            if (!event.viewer_identity)
            {
                return false;
            }
            const auto found = streaks_.find(meowlive::agent::viewer_key(event));
            return found != streaks_.end() && found->second >= 2;
        }

        unsigned non_chat_rounds() const
        {
            return non_chat_rounds_;
        }

        bool focused(const meowlive::domain::LiveEvent& event, std::uint64_t now) const
        {
            const auto* chat = std::get_if<meowlive::domain::Chat>(&event.kind);
            return focus_ && now < focus_->until && chat && event.viewer_identity &&
                   meowlive::agent::viewer_key(event) == focus_->viewer &&
                   related_to_question(focus_->question, chat->text);
        }

        void clear_focus()
        {
            focus_.reset();
        }

        void completed(const std::vector<meowlive::domain::LiveEvent>& events,
                       std::string_view assistant, std::uint64_t now)
        {
            std::unordered_set<std::string> viewers;
            bool chat{};
            for (const auto& event : events)
            {
                if (event.viewer_identity)
                {
                    viewers.insert(meowlive::agent::viewer_key(event));
                }
                chat = chat || std::holds_alternative<meowlive::domain::Chat>(event.kind);
            }
            std::erase_if(streaks_,
                          [&viewers](const auto& entry)
                          {
                              return !viewers.contains(entry.first);
                          });
            for (const auto& viewer : viewers)
            {
                auto& count = streaks_[viewer];
                count = count < 2 ? count + 1 : 2;
            }
            non_chat_rounds_ = chat ? 0 : non_chat_rounds_ < 2 ? non_chat_rounds_ + 1 : 2;
            if (focus_ && (now >= focus_->until || viewers.contains(focus_->viewer)))
            {
                focus_.reset();
            }
            const auto end = assistant.find_last_not_of(" \t\n\r\v\f");
            if (end != std::string_view::npos)
            {
                assistant = assistant.substr(0, end + 1);
            }
            if (viewers.size() == 1 &&
                std::ranges::all_of(events,
                                    [](const auto& event)
                                    {
                                        return event.viewer_identity.has_value();
                                    }) &&
                (assistant.ends_with('?') || assistant.ends_with("？")))
            {
                constexpr std::uint64_t window = 120'000;
                focus_ =
                    Focus{*viewers.begin(),
                          now + std::min(window, std::numeric_limits<std::uint64_t>::max() - now),
                          std::string(assistant)};
            }
        }

    private:
        struct Focus
        {
            std::string viewer;
            std::uint64_t until;
            std::string question;
        };

        std::deque<std::uint64_t> chats_;
        std::deque<std::uint64_t> entries_;
        std::optional<std::uint64_t> last_welcome_;
        std::unordered_map<std::string, std::uint64_t> welcomes_;
        std::unordered_map<std::string, unsigned> streaks_;
        std::optional<Focus> focus_;
        unsigned non_chat_rounds_{};
    };
}
