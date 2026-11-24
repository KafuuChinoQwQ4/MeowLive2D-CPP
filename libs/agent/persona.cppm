module;

#if defined(_WIN32)
#include <string>
#endif

export module meowlive.agent.persona;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::agent
{
    struct PersonaProfile
    {
        std::string id;
        std::string name;
        std::string persona;
    };
}
