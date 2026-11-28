module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <memory>

export module meowlive.platform.windows.handle;

export namespace meowlive::platform::windows
{
    struct CloseHandle
    {
        void operator()(void* handle) const noexcept
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(handle);
            }
        }
    };

    using UniqueHandle = std::unique_ptr<void, meowlive::platform::windows::CloseHandle>;
}
