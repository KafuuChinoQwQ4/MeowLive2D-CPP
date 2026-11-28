module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.profile_file;

import meowlive.standard;

import meowlive.adapters.storage.atomic_file;

export namespace meowlive::server
{
    bool write_profile_file(const std::filesystem::path& target,
                            const std::filesystem::path& temporary, std::string_view bytes)
    {
        return meowlive::adapters::storage::write_atomic_file(target, temporary, bytes);
    }
}
