module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.training_limits;

import meowlive.standard;

export namespace meowlive::server::transport
{
    inline constexpr std::size_t training_payload_limit = 32 * 1024 * 1024;
    inline constexpr std::size_t training_upload_limit =
        meowlive::server::transport::training_payload_limit + 128 * 1024;
}
