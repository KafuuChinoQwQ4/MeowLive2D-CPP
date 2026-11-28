module;

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.tests.server_process;

import meowlive.standard;

export namespace meowlive::tests
{
    inline constexpr std::string_view allowed_origin = "http://127.0.0.1:1420";
    inline constexpr std::string_view admin_credential = "stage-2-fixture-admin-credential";
    inline constexpr std::string_view device_credential = "stage-2-fixture-device-credential";
}

namespace meowlive::tests::process
{
    std::filesystem::path create_test_directory()
    {
        const auto root = std::filesystem::absolute(std::filesystem::temp_directory_path());
#if defined(_WIN32)
        std::random_device random;
        for (unsigned attempt = 0; attempt < 16; ++attempt)
        {
            auto candidate = root / ("meowlive-transport-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate))
            {
                return candidate;
            }
        }
#else
        auto pattern = (root / "meowlive-transport-XXXXXX").native();
        if (::mkdtemp(pattern.data()))
        {
            return pattern;
        }
#endif
        throw std::runtime_error("Cannot create integration test directory");
    }

    export struct TestDirectory
    {
        TestDirectory() = default;
        TestDirectory(const TestDirectory&) = delete;
        TestDirectory& operator=(const TestDirectory&) = delete;

        const std::filesystem::path path = create_test_directory();

        ~TestDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    export std::uint16_t available_loopback_port()
    {
#if defined(_WIN32)
        ::WSADATA winsock{};
        if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        {
            throw std::runtime_error("Cannot initialize Winsock");
        }
        const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const bool valid_socket = socket != INVALID_SOCKET;
        int address_size = sizeof(::sockaddr_in);
#else
        const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
        const bool valid_socket = socket >= 0;
        ::socklen_t address_size = sizeof(::sockaddr_in);
#endif
        ::sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        const bool bound =
            valid_socket &&
            ::bind(socket, reinterpret_cast<const ::sockaddr*>(&address), address_size) == 0 &&
            ::getsockname(socket, reinterpret_cast<::sockaddr*>(&address), &address_size) == 0;
        if (valid_socket)
        {
#if defined(_WIN32)
            ::closesocket(socket);
#else
            ::close(socket);
#endif
        }
#if defined(_WIN32)
        ::WSACleanup();
#endif
        if (!bound)
        {
            throw std::runtime_error("Cannot select a port on 127.0.0.1");
        }
        return ::ntohs(address.sin_port);
    }

    export void write_file(const std::filesystem::path& path, std::string_view contents)
    {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(path, std::ios::binary);
        file << contents;
        file.close();
    }

    void write_server_config(const std::filesystem::path& path, std::uint16_t port, bool rust,
                             bool authenticated)
    {
        auto configuration = std::format(
            "[server]\nlisten_address = \"127.0.0.1:{}\"\nallowed_origins = [\"{}\"]\n"
            "[auth]\nenabled = {}\nsession_lifetime_seconds = 2\n"
            "admin_token_env = \"\"\ndevice_token_env = \"\"\n"
            "admin_token_file = \"admin.token\"\ndevice_token_file = \"device.token\"\n",
            port, meowlive::tests::allowed_origin, authenticated);
        if (rust)
        {
            configuration += "\n[viewers]\nenabled = false\n[training]\nenabled = false\n";
        }
        if (authenticated)
        {
            write_file(path / "admin.token", meowlive::tests::admin_credential);
            write_file(path / "device.token", meowlive::tests::device_credential);
        }
        write_file(path / "server.toml", configuration);
    }

#if defined(_WIN32)
    export struct ProcessMetrics
    {
        double cpu_seconds;
        std::uint64_t working_set_bytes;
    };

    std::uint64_t file_time_ticks(const ::FILETIME& time)
    {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }
#endif

    export class ChildProcess
    {
    public:
        ChildProcess() = default;
        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        ~ChildProcess()
        {
            stop();
        }

        void start(const std::filesystem::path& executable, const std::filesystem::path& directory,
                   const std::filesystem::path& config_file, bool pass_data_dir = false,
                   const std::filesystem::path& log_file = "server.log")
        {
            const auto program_path = std::filesystem::canonical(executable);
#if defined(_WIN32)
            if (process_ != nullptr)
            {
                throw std::logic_error("Test process has already started");
            }
            // Windows supports wide paths and forbids quotes in filenames.
            auto command =
                L"\"" + program_path.native() + L"\" --config \"" + config_file.native() + L"\"";
            if (pass_data_dir)
            {
                command += L" --data-dir data";
            }
            ::SECURITY_ATTRIBUTES inheritable{sizeof(::SECURITY_ATTRIBUTES), nullptr, TRUE};
            const auto log =
                ::CreateFileW((directory / log_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            const auto input =
                ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ::STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = input;
            startup.hStdOutput = log;
            startup.hStdError = log;
            ::PROCESS_INFORMATION information{};
            const bool started = log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
                                 ::CreateProcessW(program_path.c_str(), command.data(), nullptr,
                                                  nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                                  directory.c_str(), &startup, &information);
            const auto error = ::GetLastError();
            if (input != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(input);
            }
            if (log != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(log);
            }
            if (!started)
            {
                throw std::runtime_error(
                    std::format("Cannot launch test process (Win32 {})", error));
            }
            ::CloseHandle(information.hThread);
            process_ = information.hProcess;
#else
            if (process_ > 0)
            {
                throw std::logic_error("Test process has already started");
            }
            auto program = program_path.native();
            auto working_directory = directory.native();
            char config_option[] = "--config";
            auto config = config_file.native();
            char data_option[] = "--data-dir";
            char data[] = "data";
            char* arguments[]{program.data(), config_option,
                              config.data(),  pass_data_dir ? data_option : nullptr,
                              data,           nullptr};
            const auto log_path = (directory / log_file).native();
            const auto log =
                ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (log < 0)
            {
                throw std::runtime_error("Cannot open the test process log");
            }
            const auto child = ::fork();
            if (child == 0)
            {
                if (::chdir(working_directory.c_str()) != 0 || ::dup2(log, STDOUT_FILENO) < 0 ||
                    ::dup2(log, STDERR_FILENO) < 0)
                {
                    ::_exit(126);
                }
                ::execv(program.c_str(), arguments);
                ::_exit(127);
            }
            ::close(log);
            if (child < 0)
            {
                throw std::runtime_error("Cannot launch test process");
            }
            process_ = child;
#endif
        }

        void assert_running() const
        {
            if (const auto status = exit_code())
            {
                throw std::runtime_error(std::format("Test process exited with code {}", *status));
            }
        }

        std::optional<int> exit_code() const
        {
#if defined(_WIN32)
            const auto state = ::WaitForSingleObject(process_, 0);
            if (state == WAIT_TIMEOUT)
            {
                return std::nullopt;
            }
            DWORD status{};
            if (state == WAIT_OBJECT_0 && ::GetExitCodeProcess(process_, &status))
            {
                return static_cast<int>(status);
            }
#else
            ::siginfo_t information{};
            if (::waitid(P_PID, static_cast<::id_t>(process_), &information,
                         WEXITED | WNOHANG | WNOWAIT) == 0)
            {
                if (information.si_pid == 0)
                {
                    return std::nullopt;
                }
                if (information.si_code == CLD_EXITED)
                {
                    return information.si_status;
                }
                throw std::runtime_error(
                    std::format("Test process terminated with signal {}", information.si_status));
            }
#endif
            throw std::runtime_error("Cannot determine the test process state");
        }

#if defined(_WIN32)
        meowlive::tests::process::ProcessMetrics metrics() const
        {
            assert_running();
            ::FILETIME created{}, exited{}, kernel{}, user{};
            ::PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof(memory);
            if (!::GetProcessTimes(process_, &created, &exited, &kernel, &user) ||
                !::K32GetProcessMemoryInfo(process_, &memory, sizeof(memory)))
            {
                throw std::runtime_error(
                    std::format("Cannot sample test process metrics (Win32 {})", ::GetLastError()));
            }
            const auto cpu_ticks = meowlive::tests::process::file_time_ticks(kernel) +
                                   meowlive::tests::process::file_time_ticks(user);
            return {static_cast<double>(cpu_ticks) / 10'000'000.0, memory.WorkingSetSize};
        }
#endif

    private:
        void stop() noexcept
        {
#if defined(_WIN32)
            if (process_ != nullptr)
            {
                if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
                {
                    ::TerminateProcess(process_, 1);
                    ::WaitForSingleObject(process_, 5000);
                }
                ::CloseHandle(process_);
            }
#else
            if (process_ <= 0)
            {
                return;
            }
            ::kill(process_, SIGTERM);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            int status{};
            do
            {
                const auto result = ::waitpid(process_, &status, WNOHANG);
                if (result == process_ || (result < 0 && errno != EINTR))
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } while (std::chrono::steady_clock::now() < deadline);
            ::kill(process_, SIGKILL);
            while (::waitpid(process_, &status, 0) < 0 && errno == EINTR)
            {
            }
#endif
        }

#if defined(_WIN32)
        ::HANDLE process_ = nullptr;
#else
        ::pid_t process_ = -1;
#endif
    };
}

export namespace meowlive::tests
{
    class ServerProcess
    {
    public:
        ServerProcess(const std::filesystem::path& executable, bool rust,
                      bool authenticated = false)
            : port_(meowlive::tests::process::available_loopback_port())
        {
            meowlive::tests::process::write_server_config(directory_.path, port_, rust,
                                                          authenticated);
            child_.start(executable, directory_.path, "server.toml", !rust);
        }

        ~ServerProcess()
        {
            if (std::uncaught_exceptions() > exceptions_)
            {
                try
                {
                    std::ifstream log(directory_.path / "server.log", std::ios::binary);
                    std::string contents(4096, '\0');
                    log.read(contents.data(), contents.size());
                    contents.resize(static_cast<std::size_t>(log.gcount()));
                    if (!contents.empty())
                    {
                        std::println(stderr, "Test server output:\n{}", contents);
                    }
                }
                catch (...)
                {
                }
            }
        }

        std::uint16_t port() const noexcept
        {
            return port_;
        }

        void assert_running() const
        {
            child_.assert_running();
        }

    private:
        meowlive::tests::process::TestDirectory directory_;
        const std::uint16_t port_;
        const int exceptions_ = std::uncaught_exceptions();
        meowlive::tests::process::ChildProcess child_;
    };
}
