module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.version;

import meowlive.standard;

export namespace meowlive::protocol
{
    inline constexpr std::uint16_t version{3};
}
