module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.obs_status;

import meowlive.standard;

export namespace meowlive::application
{
    struct ObsStatus
    {
        bool connected{};
        bool recording{};
        std::string current_scene;
    };

    using ObsStatusQuery = std::function<std::optional<meowlive::application::ObsStatus>(
        std::chrono::seconds timeout, std::stop_token stop)>;
}
