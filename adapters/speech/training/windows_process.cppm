module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#endif

export module meowlive.adapters.speech.windows_process;

#if defined(_WIN32)
import meowlive.platform.windows.handle;

export namespace meowlive::adapters::speech
{
    enum class ProcessError
    {
        Start,
        Cancelled,
        Timeout,
        Output,
        Exit,
    };
}

namespace
{
    namespace speech = meowlive::adapters::speech;

    std::wstring quote(std::wstring_view argument)
    {
        std::wstring encoded(1, L'"');
        std::size_t backslashes{};
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                encoded.append(backslashes + 1, L'\\');
            }
            encoded.append(backslashes, L'\\');
            encoded.push_back(character);
            backslashes = 0;
        }
        encoded.append(backslashes * 2, L'\\');
        encoded.push_back(L'"');
        return encoded;
    }

    class InheritedHandles
    {
    public:
        explicit InheritedHandles(std::span<HANDLE> inheritable)
        {
            SIZE_T storage_size{};
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &storage_size);
            if (!storage_size)
            {
                return;
            }
            storage_.resize(storage_size);
            auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
            if (!::InitializeProcThreadAttributeList(candidate, 1, 0, &storage_size))
            {
                return;
            }
            handles_ = candidate;
            valid_ = ::UpdateProcThreadAttribute(handles_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                 inheritable.data(), inheritable.size_bytes(),
                                                 nullptr, nullptr) != FALSE;
        }

        ~InheritedHandles()
        {
            if (handles_)
            {
                ::DeleteProcThreadAttributeList(handles_);
            }
        }

        [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept
        {
            return valid_ ? handles_ : nullptr;
        }

    private:
        std::vector<std::byte> storage_;
        LPPROC_THREAD_ATTRIBUTE_LIST handles_{};
        bool valid_{};
    };

    meowlive::platform::windows::UniqueHandle create_process_job()
    {
        meowlive::platform::windows::UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION policy{};
        policy.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                               &policy, sizeof(policy)))
        {
            return {};
        }
        return job;
    }

    bool read_available(HANDLE pipe, std::string& output, std::size_t limit,
                        const std::function<void(std::string_view)>& on_output)
    {
        std::array<char, 4096> buffer{};
        while (true)
        {
            DWORD available{};
            if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                return ::GetLastError() == ERROR_BROKEN_PIPE;
            }
            if (available == 0)
            {
                return true;
            }
            if (available > limit - output.size())
            {
                return false;
            }
            DWORD received{};
            if (!::ReadFile(pipe, buffer.data(),
                            std::min(available, static_cast<DWORD>(buffer.size())), &received,
                            nullptr) ||
                received == 0)
            {
                return false;
            }
            output.append(buffer.data(), received);
            if (on_output)
            {
                on_output(std::string_view(buffer.data(), received));
            }
        }
    }
}

export namespace meowlive::adapters::speech
{
    std::expected<std::string, meowlive::adapters::speech::ProcessError>
    run_windows_process(const std::filesystem::path& executable, std::span<const std::wstring> args,
                        const std::filesystem::path& directory, std::chrono::milliseconds timeout,
                        std::size_t output_limit, std::stop_token stop,
                        const std::function<void(std::string_view)>& on_output = {})
    {
        if (stop.stop_requested())
        {
            return std::unexpected(speech::ProcessError::Cancelled);
        }
        if (!executable.is_absolute() || !directory.is_absolute())
        {
            return std::unexpected(speech::ProcessError::Start);
        }

        auto command = quote(executable.native());
        for (const auto& arg : args)
        {
            command.push_back(L' ');
            command += quote(arg);
        }
        if (command.size() >= 32'767)
        {
            return std::unexpected(speech::ProcessError::Start);
        }
        SECURITY_ATTRIBUTES share{sizeof(share), nullptr, TRUE};
        HANDLE read{};
        HANDLE write{};
        if (!::CreatePipe(&read, &write, &share, 0))
        {
            return std::unexpected(speech::ProcessError::Start);
        }
        const meowlive::platform::windows::UniqueHandle reader(read);
        meowlive::platform::windows::UniqueHandle writer(write);
        const meowlive::platform::windows::UniqueHandle null_input(
            ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &share,
                          OPEN_EXISTING, 0, nullptr));
        const meowlive::platform::windows::UniqueHandle null_error(
            ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &share,
                          OPEN_EXISTING, 0, nullptr));
        std::array handles{writer.get(), null_input.get(), null_error.get()};
        if (!::SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0) ||
            null_input.get() == INVALID_HANDLE_VALUE || null_error.get() == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(speech::ProcessError::Start);
        }
        InheritedHandles attributes(handles);
        const auto job = create_process_job();
        if (!attributes.get() || !job)
        {
            return std::unexpected(speech::ProcessError::Start);
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_input.get();
        startup.StartupInfo.hStdOutput = writer.get();
        startup.StartupInfo.hStdError = null_error.get();
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION spawned{};
        if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                              nullptr, directory.c_str(), &startup.StartupInfo, &spawned))
        {
            return std::unexpected(speech::ProcessError::Start);
        }

        const meowlive::platform::windows::UniqueHandle process(spawned.hProcess);
        const meowlive::platform::windows::UniqueHandle thread(spawned.hThread);
        writer.reset();
        if (!::AssignProcessToJobObject(job.get(), process.get()))
        {
            ::TerminateProcess(process.get(), 1);
            ::WaitForSingleObject(process.get(), 1000);
            return std::unexpected(speech::ProcessError::Start);
        }
        if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        {
            return std::unexpected(speech::ProcessError::Start);
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string output;
        while (!stop.stop_requested())
        {
            if (!read_available(reader.get(), output, output_limit, on_output))
            {
                return std::unexpected(speech::ProcessError::Output);
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return std::unexpected(speech::ProcessError::Timeout);
            }
            const auto result = ::WaitForSingleObject(process.get(), 25);
            if (result == WAIT_OBJECT_0)
            {
                DWORD code{};
                if (!::GetExitCodeProcess(process.get(), &code) || code != 0)
                {
                    return std::unexpected(speech::ProcessError::Exit);
                }
                if (!read_available(reader.get(), output, output_limit, on_output))
                {
                    return std::unexpected(speech::ProcessError::Output);
                }
                return output;
            }
            if (result != WAIT_TIMEOUT)
            {
                return std::unexpected(speech::ProcessError::Exit);
            }
        }
        return std::unexpected(speech::ProcessError::Cancelled);
    }
}
#endif
