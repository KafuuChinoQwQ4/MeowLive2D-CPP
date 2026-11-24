module;

#if defined(_WIN32)
#include <cstdint>
#endif

export module meowlive.protocol.version;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::protocol
{
    inline constexpr std::uint16_t version{3};
}
