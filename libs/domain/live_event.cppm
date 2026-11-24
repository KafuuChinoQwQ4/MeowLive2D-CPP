module;

#if defined(_WIN32)
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#endif

export module meowlive.domain.live_event;

#if !defined(_WIN32)
import std;
#endif

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
}
