module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.atomic_file;

import meowlive.standard;
#if defined(_WIN32)
import meowlive.platform.windows.handle;
#endif

namespace
{
    bool replace_file(const std::filesystem::path& temporary, const std::filesystem::path& target,
                      std::string_view bytes) noexcept
    {
#if defined(_WIN32)
        meowlive::platform::windows::UniqueHandle handle(
            ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written{};
        const bool flushed = ::WriteFile(handle.get(), bytes.data(),
                                         static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                             written == bytes.size() && ::FlushFileBuffers(handle.get());
        const bool closed = ::CloseHandle(handle.release());
        return flushed && closed &&
               ::MoveFileExW(temporary.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
        const int fd = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0)
        {
            return false;
        }
        while (!bytes.empty())
        {
            const auto count = ::write(fd, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                break;
            }
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        const bool flushed = bytes.empty() && ::fsync(fd) == 0;
        const bool closed = ::close(fd) == 0;
        return flushed && closed && ::rename(temporary.c_str(), target.c_str()) == 0;
#endif
    }
}

export namespace meowlive::adapters::storage
{
    bool write_atomic_file(const std::filesystem::path& target,
                           const std::filesystem::path& temporary, std::string_view bytes)
    {
        std::error_code error;
        const auto directory = target.parent_path();
        std::filesystem::create_directories(directory, error);
        if (error ||
            !std::filesystem::is_directory(std::filesystem::symlink_status(directory, error)))
        {
            return false;
        }
        const auto status = std::filesystem::symlink_status(target, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)))
        {
            return false;
        }
        if (replace_file(temporary, target, bytes))
        {
            return true;
        }
        std::filesystem::remove(temporary, error);
        return false;
    }
}
