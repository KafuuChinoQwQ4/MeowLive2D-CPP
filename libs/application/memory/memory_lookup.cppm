module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.memory_lookup;

import meowlive.standard;

import meowlive.domain.live_event;

export namespace meowlive::application
{
    using ViewerMemoryQuery = std::function<std::expected<std::string, std::string>(
        const meowlive::domain::LiveEvent&, std::chrono::seconds, std::stop_token)>;
}
