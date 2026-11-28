module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.live;

import meowlive.standard;

import meowlive.domain.live_event;

export namespace meowlive::application
{
    struct LiveCredentials
    {
        std::uint64_t app_id{};
        std::string access_key_id;
        std::string access_key_secret;
        std::string identity_code;
    };

    enum class LiveSourceFailure
    {
        Stopped,
        Authorization,
        Disconnected,
        Cleanup,
        InternalError,
    };

    // Callbacks run synchronously on the source thread and expire when the source returns.
    using LiveSource = std::function<meowlive::application::LiveSourceFailure(
        const meowlive::application::LiveCredentials&, std::stop_token,
        const std::function<void(std::string)>&,
        const std::function<void(meowlive::domain::LiveEvent)>&, const std::function<void()>&)>;
}
