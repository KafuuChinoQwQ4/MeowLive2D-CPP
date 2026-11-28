module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.reply_completion;

import meowlive.standard;

export namespace meowlive::application
{
    struct ReplyEventKey
    {
        std::string source;
        std::string id;
    };

    struct ReplyCompletion
    {
        std::string speech_id;
        std::uint64_t generated_at_ms{};
        std::uint64_t completed_at_ms{};
        std::vector<meowlive::application::ReplyEventKey> events;
    };

    enum class ReplyReservationError
    {
        Full,
        Closed,
        Invalid,
    };

    // Reserve and confirm perform no storage I/O. Destroying an unconfirmed reservation
    // releases its capacity. A confirmed record belongs to the outbox until persisted.
    class ReplyReservation
    {
    public:
        virtual ~ReplyReservation() = default;
        virtual void confirm(std::uint64_t completed_at_ms) noexcept = 0;
    };

    using ReplyReserveResult =
        std::expected<std::unique_ptr<meowlive::application::ReplyReservation>,
                      meowlive::application::ReplyReservationError>;
    using ReserveReply = std::function<meowlive::application::ReplyReserveResult(
        meowlive::application::ReplyCompletion)>;
}
