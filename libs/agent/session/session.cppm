module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.agent.session;

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.agent.runtime;
import meowlive.application.llm;
import meowlive.application.reply_completion;
import meowlive.application.scheduler;
import meowlive.domain.live_event;
import meowlive.domain.speech;

export namespace meowlive::agent
{
    struct AgentSettings
    {
        std::string persona{"你是一位友好、自然的 Live2D 主播，用简短中文与观众互动。"};
        std::string system_prompt;
        std::string topic;
        bool proactive_enabled{};
        std::uint64_t cooldown_ms{30'000};
        meowlive::agent::InteractionSettings interaction;
    };

    struct AgentLimits
    {
        std::size_t pending_capacity{128};
        std::size_t history_limit{200};
        std::size_t dedup_capacity{512};
        std::uint64_t event_ttl_ms{60'000};
        std::uint64_t room_enter_ttl_ms{60'000};
        std::size_t batch_size{8};
        std::size_t conversation_limit{6};
    };

    enum class EventStatus
    {
        Pending,
        Deciding,
        Queued,
        Synthesizing,
        Ready,
        Playing,
        Completed,
        Cancelled,
        Failed,
        Expired,
        Skipped,
        Unknown,
    };

    constexpr std::string_view event_status_name(meowlive::agent::EventStatus status)
    {
        switch (status)
        {
        case meowlive::agent::EventStatus::Pending:
            return "pending";
        case meowlive::agent::EventStatus::Deciding:
            return "deciding";
        case meowlive::agent::EventStatus::Queued:
            return "queued";
        case meowlive::agent::EventStatus::Synthesizing:
            return "synthesizing";
        case meowlive::agent::EventStatus::Ready:
            return "ready";
        case meowlive::agent::EventStatus::Playing:
            return "playing";
        case meowlive::agent::EventStatus::Completed:
            return "completed";
        case meowlive::agent::EventStatus::Cancelled:
            return "cancelled";
        case meowlive::agent::EventStatus::Failed:
            return "failed";
        case meowlive::agent::EventStatus::Expired:
            return "expired";
        case meowlive::agent::EventStatus::Skipped:
            return "skipped";
        case meowlive::agent::EventStatus::Unknown:
            return "unknown";
        }
        return "unknown";
    }

    struct EventRecord
    {
        meowlive::domain::LiveEvent event;
        meowlive::agent::EventStatus status{meowlive::agent::EventStatus::Pending};
        std::uint64_t deadline{};
        std::string speech_id;
        std::string error;
        unsigned skips{};
        bool persisted{};
    };

    struct DecisionWork
    {
        std::uint64_t id{};
        meowlive::application::DecisionRequest request;
        std::vector<std::string> memory_event_ids;
    };

    struct PreparedSpeech
    {
        std::vector<meowlive::domain::LiveEvent> events;
        meowlive::application::ConversationTurn turn;
        std::vector<meowlive::application::ReplyEventKey> persisted_events;
    };

    struct Submission
    {
        unsigned accepted{};
        unsigned duplicates{};
        unsigned unscheduled{};
    };

    class AgentSession
    {
    public:
        explicit AgentSession(meowlive::agent::AgentLimits limits = {}) : limits_(limits)
        {
        }

        void configure(meowlive::agent::AgentSettings settings, std::uint64_t now)
        {
            set_paused(true, now);
            if (settings_.persona != settings.persona ||
                settings_.system_prompt != settings.system_prompt)
            {
                history_.clear();
                interaction_.clear_focus();
                narrowed_.reset();
                for (auto& row : records_)
                {
                    row.skips = 0;
                }
                ++context_;
            }
            settings_ = std::move(settings);
        }

        void set_paused(bool paused, std::uint64_t now)
        {
            if (paused && flight_)
            {
                release(*flight_, meowlive::agent::EventStatus::Pending, {});
                flight_.reset();
            }
            if (paused_ && !paused)
            {
                proactive_after_ = now + settings_.cooldown_ms;
            }
            paused_ = paused;
            expire(now);
        }

        void stop(std::uint64_t now)
        {
            set_paused(true, now);
            for (auto& row : records_)
            {
                if (row.status == meowlive::agent::EventStatus::Pending)
                {
                    row.status = meowlive::agent::EventStatus::Cancelled;
                }
            }
            prune();
        }

        void invalidate_conversation(std::uint64_t now)
        {
            set_paused(true, now);
            history_.clear();
            interaction_.clear_focus();
            narrowed_.reset();
            ++context_;
        }

        void discard_pending(std::string_view source)
        {
            for (auto& row : records_)
            {
                if (row.status == meowlive::agent::EventStatus::Pending &&
                    row.event.source == source)
                {
                    row.status = meowlive::agent::EventStatus::Cancelled;
                }
            }
            prune();
        }

        meowlive::agent::Submission submit(std::vector<meowlive::domain::LiveEvent> events,
                                           std::uint64_t now, std::uint64_t wall,
                                           std::optional<std::uint64_t> deadline = std::nullopt,
                                           bool persisted = false);

        std::optional<meowlive::agent::DecisionWork> begin(std::uint64_t now);

        std::expected<std::optional<meowlive::agent::PreparedSpeech>, std::string>
        resolve(std::uint64_t id, meowlive::application::AgentDecision decision, std::uint64_t now);

        void fail(std::uint64_t id, std::string message, std::uint64_t now);

        void enqueued(meowlive::agent::PreparedSpeech prepared,
                      const meowlive::application::SpeechTask& task, std::uint64_t now);

        void enqueue_failed(const meowlive::agent::PreparedSpeech& prepared, std::string message);

        void sync(const meowlive::application::SpeechTask& task, std::uint64_t now);

        void expire(std::uint64_t now);

        meowlive::agent::AgentPhase phase() const
        {
            using Phase = meowlive::agent::AgentPhase;
            return paused_     ? Phase::Paused
                   : playback_ ? Phase::Speaking
                   : flight_   ? Phase::Deciding
                               : Phase::Waiting;
        }

        const meowlive::agent::AgentSettings& settings() const
        {
            return settings_;
        }
        const std::deque<meowlive::agent::EventRecord>& events() const
        {
            return records_;
        }
        const std::deque<meowlive::application::ConversationTurn>& history() const
        {
            return history_;
        }
        const std::string& last_error() const
        {
            return last_error_;
        }
        std::string speech_id() const
        {
            return playback_ ? playback_->id : std::string{};
        }
        std::uint64_t cooldown_until() const
        {
            return cooldown_until_;
        }

    private:
        struct Flight
        {
            std::uint64_t id;
            std::vector<std::vector<std::string>> groups;
            std::optional<std::string> preferred;
            bool required{};
        };

        struct Selection
        {
            std::vector<std::vector<meowlive::agent::EventRecord*>> groups;
            std::optional<std::string> preferred;
            bool read_all{};

            bool empty() const
            {
                return groups.empty();
            }
        };

        struct Playback
        {
            std::string id;
            std::uint32_t generation;
            std::uint64_t context;
            meowlive::agent::PreparedSpeech prepared;
        };

        static meowlive::agent::EventStatus event_status(meowlive::domain::SpeechStatus status);

        static bool active_row(const meowlive::agent::EventRecord& row);

        static unsigned admission_priority(const meowlive::domain::LiveEvent& event);

        bool make_room(const meowlive::domain::LiveEvent& incoming);

        auto find(const std::string& id) -> std::deque<meowlive::agent::EventRecord>::iterator
        {
            return std::ranges::find_if(records_,
                                        [&id](const auto& row)
                                        {
                                            return row.event.id == id;
                                        });
        }

        std::vector<meowlive::agent::EventRecord*> flight_rows(const Flight& flight)
        {
            std::vector<meowlive::agent::EventRecord*> rows;
            for (const auto& group : flight.groups)
            {
                for (const auto& id : group)
                {
                    rows.push_back(&*find(id));
                }
            }
            return rows;
        }

        void release(const Flight& flight, meowlive::agent::EventStatus status,
                     const std::string& error);

        void cooldown(std::uint64_t now);

        void prune();

        Selection select(std::uint64_t now);
        std::optional<std::string>
        prioritize_candidates(std::vector<meowlive::agent::EventRecord*>& pending,
                              std::uint64_t now) const;
        void group_candidates(std::vector<meowlive::agent::EventRecord*>& pending,
                              Selection& result) const;

        meowlive::agent::AgentLimits limits_;
        meowlive::agent::AgentSettings settings_;
        meowlive::agent::Interaction interaction_;
        std::deque<meowlive::agent::EventRecord> records_;
        std::unordered_set<std::string> seen_;
        std::deque<std::string> seen_order_;
        std::deque<meowlive::application::ConversationTurn> history_;
        std::optional<Flight> flight_;
        std::optional<Playback> playback_;
        std::optional<std::string> narrowed_;
        std::string last_error_;
        std::uint64_t next_id_{};
        std::uint64_t context_{};
        std::uint64_t cooldown_until_{};
        std::uint64_t proactive_after_{};
        bool paused_{true};
    };
}
