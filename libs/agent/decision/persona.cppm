module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.agent.persona;

import meowlive.standard;

export namespace meowlive::agent
{
    struct PersonaProfile
    {
        std::string id;
        std::string name;
        std::string persona;
    };
}
