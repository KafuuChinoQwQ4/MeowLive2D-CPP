module;

#include <cstdio>

#include <meowlive/msvc_stl.hpp>

export module meowlive.platform.exception_diagnostics;

import meowlive.standard;

export namespace meowlive::platform
{
    enum class ExceptionKind
    {
        OutOfMemory,
        LogicError,
        SystemError,
        RuntimeError,
        StandardException,
        Unknown,
    };

    enum class ExceptionDetails
    {
        Include,
        Omit,
    };

    using ExceptionLogOutput = void (*)(std::string_view) noexcept;
}

namespace
{
    std::atomic<meowlive::platform::ExceptionLogOutput> output{};
    std::atomic<std::FILE*> log_file{};

    void write_line(std::string_view line) noexcept
    {
        if (const auto sink = output.load())
        {
            sink(line);
            return;
        }
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
        if (auto* file = log_file.load())
        {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fflush(file);
        }
    }

    meowlive::platform::ExceptionKind
    report(meowlive::platform::ExceptionKind kind, std::string_view operation, const char* category,
           const std::exception* error, meowlive::platform::ExceptionDetails details,
           std::source_location source, const std::error_code* code = nullptr) noexcept
    {
        std::array<char, 512> message{};
        if (error && details == meowlive::platform::ExceptionDetails::Include)
        {
            const auto* description = error->what();
            for (std::size_t index = 0; index < message.size() - 1 && description[index]; ++index)
            {
                const auto character = static_cast<unsigned char>(description[index]);
                message[index] = character < 0x20 || character == 0x7f ? ' ' : description[index];
            }
        }
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        std::array<char, 2'048> line;
        const auto path = std::string_view(source.file_name());
        const auto filename = path.substr(path.find_last_of("/\\") + 1);
        std::array<char, 128> error_code{};
        if (code)
        {
            std::snprintf(error_code.data(), error_code.size(), " code=%.64s:%d",
                          code->category().name(), code->value());
        }
        const auto size =
            std::snprintf(line.data(), line.size(),
                          "ERROR time_unix_ms=%lld operation=%.*s exception=%s type=%.256s "
                          "source=%.*s:%u%s%s%s\n",
                          static_cast<long long>(timestamp),
                          static_cast<int>(std::min(operation.size(), std::size_t{256})),
                          operation.empty() ? "" : operation.data(), category,
                          error ? typeid(*error).name() : "unknown",
                          static_cast<int>(std::min(filename.size(), std::size_t{256})),
                          filename.data(), static_cast<unsigned int>(source.line()),
                          error_code.data(), message.front() ? " what=" : "", message.data());
        if (size > 0)
        {
            write_line(std::string_view(line.data(),
                                        std::min(static_cast<std::size_t>(size), line.size() - 1)));
        }
        return kind;
    }
}

export namespace meowlive::platform
{
    void set_exception_log_output(meowlive::platform::ExceptionLogOutput sink) noexcept
    {
        output.store(sink);
    }

    // Keep the file open through thread shutdown and flush each record synchronously.
    bool open_exception_log(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        auto* file = ::_wfopen(path.c_str(), L"ab");
#else
        auto* file = std::fopen(path.c_str(), "ab");
#endif
        if (!file)
        {
            return false;
        }
        if (std::setvbuf(file, nullptr, _IONBF, 0) != 0)
        {
            std::fclose(file);
            return false;
        }
        std::FILE* previous = nullptr;
        if (!log_file.compare_exchange_strong(previous, file))
        {
            std::fclose(file);
            return false;
        }
        return true;
    }

    // Call from an active exception handler. Include what() only when it cannot contain private
    // data.
    meowlive::platform::ExceptionKind
    log_current_exception(std::string_view operation,
                          meowlive::platform::ExceptionDetails details =
                              meowlive::platform::ExceptionDetails::Include,
                          std::source_location source = std::source_location::current()) noexcept
    {
        try
        {
            throw;
        }
        catch (const std::bad_alloc& error)
        {
            return report(meowlive::platform::ExceptionKind::OutOfMemory, operation,
                          "std::bad_alloc", &error, details, source);
        }
        catch (const std::logic_error& error)
        {
            return report(meowlive::platform::ExceptionKind::LogicError, operation,
                          "std::logic_error", &error, details, source);
        }
        catch (const std::system_error& error)
        {
            return report(meowlive::platform::ExceptionKind::SystemError, operation,
                          "std::system_error", &error, details, source, &error.code());
        }
        catch (const std::runtime_error& error)
        {
            return report(meowlive::platform::ExceptionKind::RuntimeError, operation,
                          "std::runtime_error", &error, details, source);
        }
        catch (const std::exception& error)
        {
            return report(meowlive::platform::ExceptionKind::StandardException, operation,
                          "std::exception", &error, details, source);
        }
        catch (...)
        {
            return report(meowlive::platform::ExceptionKind::Unknown, operation, "unknown", nullptr,
                          details, source);
        }
    }

    void install_terminate_handler() noexcept
    {
        std::set_terminate(
            []() noexcept
            {
                if (std::current_exception())
                {
                    meowlive::platform::log_current_exception(
                        "std::terminate", meowlive::platform::ExceptionDetails::Omit);
                }
                else
                {
                    write_line("ERROR operation=std::terminate exception=none\n");
                }
                std::abort();
            });
    }
}
