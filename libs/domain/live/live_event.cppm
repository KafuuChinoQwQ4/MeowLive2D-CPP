module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.domain.live_event;

import meowlive.standard;

export namespace meowlive::domain
{
    enum class ViewerIdentityKind
    {
        OpenId,
        Uid,
    };

    struct ViewerIdentity
    {
        std::string identity_namespace;
        meowlive::domain::ViewerIdentityKind kind{};
        std::string external_id;
    };

    struct GiftMetadata
    {
        std::optional<std::uint64_t> price;
        std::optional<bool> paid;
        std::optional<std::uint32_t> medal_level;
        std::optional<std::uint32_t> guard_level;
    };

    struct Chat
    {
        std::string text;
    };

    struct Gift
    {
        std::string name;
        std::uint32_t count{};
    };

    struct SuperChat
    {
        std::string text;
        std::uint32_t amount_cny{};
        std::uint64_t start_at_ms{};
        std::uint64_t end_at_ms{};
    };

    struct RoomEnter
    {
    };

    using EventKind = std::variant<meowlive::domain::Chat, meowlive::domain::Gift,
                                   meowlive::domain::SuperChat, meowlive::domain::RoomEnter>;

    struct LiveEvent
    {
        std::string id;
        std::string source;
        std::string viewer;
        std::optional<meowlive::domain::ViewerIdentity> viewer_identity;
        std::uint64_t occurred_at_ms{};
        std::optional<meowlive::domain::GiftMetadata> gift_metadata;
        meowlive::domain::EventKind kind;
    };

    inline std::uint64_t response_lifetime_ms(const meowlive::domain::LiveEvent& event,
                                              std::uint64_t utc_ms,
                                              std::uint64_t event_ttl_ms = 60'000,
                                              std::uint64_t room_enter_ttl_ms = 60'000)
    {
        if (const auto* sc = std::get_if<meowlive::domain::SuperChat>(&event.kind))
        {
            return sc->end_at_ms > utc_ms
                       ? std::min(sc->end_at_ms - utc_ms, std::uint64_t{86'400'000})
                       : 0;
        }
        const auto ttl = std::holds_alternative<meowlive::domain::RoomEnter>(event.kind)
                             ? room_enter_ttl_ms
                             : event_ttl_ms;
        const auto age = event.occurred_at_ms && event.occurred_at_ms < utc_ms
                             ? utc_ms - event.occurred_at_ms
                             : 0;
        return ttl - std::min(ttl, age);
    }
}
