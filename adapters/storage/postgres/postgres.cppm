module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.postgres;

import meowlive.standard;

export namespace meowlive::adapters::storage
{
    class PostgresError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
        ~PostgresError() override;
    };

    class PostgresUnavailable : public meowlive::adapters::storage::PostgresError
    {
    public:
        using meowlive::adapters::storage::PostgresError::PostgresError;
        ~PostgresUnavailable() override;
    };
}

namespace meowlive::adapters::storage
{
    PostgresError::~PostgresError() = default;
    PostgresUnavailable::~PostgresUnavailable() = default;
}

namespace meowlive::adapters::storage::postgres_detail
{
    struct PgConnection;
    struct PgResult;

    bool retryable_sqlstate(std::string_view code)
    {
        return code == "08003" || code == "08006" || code == "40001" || code == "40P01" ||
               code == "55P03" || code == "57014" || code == "57P01" || code == "57P02" ||
               code == "57P03" || code == "53300";
    }

    class Driver
    {
    public:
        explicit Driver(const std::filesystem::path& library)
        {
#if defined(_WIN32)
            if (library.is_absolute())
            {
                handle_ = ::LoadLibraryExW(library.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
#else
            handle_ =
                ::dlopen(library.empty() ? "libpq.so.5" : library.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
            if (!handle_)
            {
                throw meowlive::adapters::storage::PostgresError("postgres_driver_unavailable");
            }
        }

        ~Driver()
        {
#if defined(_WIN32)
            ::FreeLibrary(handle_);
#else
            ::dlclose(handle_);
#endif
        }

        Driver(const Driver&) = delete;
        Driver& operator=(const Driver&) = delete;

        template <typename Function> Function load(const char* name) const
        {
#if defined(_WIN32)
            const auto symbol = ::GetProcAddress(handle_, name);
#else
            const auto symbol = ::dlsym(handle_, name);
#endif
            if (!symbol)
            {
                throw meowlive::adapters::storage::PostgresError("postgres_driver_incompatible");
            }
            return reinterpret_cast<Function>(symbol);
        }

    private:
#if defined(_WIN32)
        ::HMODULE handle_{};
#else
        void* handle_{};
#endif
    };
}

export namespace meowlive::adapters::storage
{
    using PostgresRows = std::vector<std::vector<std::optional<std::string>>>;

    class PostgresConnection
    {
    public:
        explicit PostgresConnection(const std::string& url,
                                    const std::filesystem::path& library = {},
                                    unsigned connect_timeout_seconds = 5)
            : driver_(library), connect_(driver_.load<Connect>("PQconnectdbParams")),
              status_(driver_.load<Status>("PQstatus")),
              connection_(nullptr, driver_.load<Finish>("PQfinish")),
              exec_(driver_.load<Exec>("PQexec")),
              exec_params_(driver_.load<ExecParams>("PQexecParams")),
              clear_(driver_.load<Clear>("PQclear")),
              result_status_(driver_.load<ResultStatus>("PQresultStatus")),
              error_field_(driver_.load<ErrorField>("PQresultErrorField")),
              rows_(driver_.load<Size>("PQntuples")), columns_(driver_.load<Size>("PQnfields")),
              is_null_(driver_.load<IsNull>("PQgetisnull")),
              value_(driver_.load<Value>("PQgetvalue")),
              set_notice_processor_(driver_.load<SetNoticeProcessor>("PQsetNoticeProcessor"))
        {
            if (url.empty() || url.find('\0') != std::string::npos)
            {
                throw meowlive::adapters::storage::PostgresError(
                    "postgres_connection_config_invalid");
            }
            const char* keywords[] = {"dbname", "connect_timeout", nullptr};
            const auto timeout = std::to_string(connect_timeout_seconds);
            const char* values[] = {url.c_str(), timeout.c_str(), nullptr};
            connection_.reset(connect_(keywords, values, 1));
            if (!connection_)
            {
                throw std::bad_alloc{};
            }
            if (status_(connection_.get()) != 0)
            {
                throw meowlive::adapters::storage::PostgresUnavailable(
                    "postgres_connection_failed");
            }
            set_notice_processor_(
                connection_.get(),
                [](void*, const char*)
                {
                },
                nullptr);
        }

        meowlive::adapters::storage::PostgresRows
        execute(const std::string& sql, std::span<const std::string> arguments = {})
        {
            std::vector<const char*> values;
            values.reserve(arguments.size());
            for (const auto& argument : arguments)
            {
                if (argument.find('\0') != std::string::npos)
                {
                    throw std::invalid_argument("postgres_parameter_invalid");
                }
                values.push_back(argument.c_str());
            }

            const std::unique_ptr<PgResult, Clear> result(
                arguments.empty()
                    ? exec_(connection_.get(), sql.c_str())
                    : exec_params_(connection_.get(), sql.c_str(), static_cast<int>(values.size()),
                                   nullptr, values.data(), nullptr, nullptr, 0),
                clear_);
            if (!result)
            {
                if (status_(connection_.get()) != 0)
                {
                    throw meowlive::adapters::storage::PostgresUnavailable(
                        "postgres_connection_lost");
                }
                throw meowlive::adapters::storage::PostgresError("postgres_query_result_missing");
            }
            constexpr int command_ok = 1;
            constexpr int tuples_ok = 2;
            const auto status = result_status_(result.get());
            if (status != command_ok && status != tuples_ok)
            {
                const auto* field = error_field_(result.get(), 'C');
                const auto code = field ? std::string_view(field) : std::string_view{};
                const bool has_code =
                    code.size() == 5 &&
                    code.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") ==
                        std::string_view::npos;
                const auto message = has_code ? "postgres_query_failed_" + std::string(code)
                                              : "postgres_query_failed";
                if (has_code
                        ? meowlive::adapters::storage::postgres_detail::retryable_sqlstate(code)
                        : status_(connection_.get()) != 0)
                {
                    throw meowlive::adapters::storage::PostgresUnavailable(message);
                }
                throw meowlive::adapters::storage::PostgresError(message);
            }

            meowlive::adapters::storage::PostgresRows output;
            const auto rows = rows_(result.get());
            const auto columns = columns_(result.get());
            output.reserve(static_cast<std::size_t>(rows));
            for (int row = 0; row < rows; ++row)
            {
                auto& record = output.emplace_back();
                record.reserve(static_cast<std::size_t>(columns));
                for (int column = 0; column < columns; ++column)
                {
                    if (is_null_(result.get(), row, column))
                    {
                        record.emplace_back(std::nullopt);
                    }
                    else
                    {
                        record.emplace_back(value_(result.get(), row, column));
                    }
                }
            }
            return output;
        }

    private:
        using PgConnection = meowlive::adapters::storage::postgres_detail::PgConnection;
        using PgResult = meowlive::adapters::storage::postgres_detail::PgResult;
        using Connect = PgConnection* (*)(const char* const*, const char* const*, int);
        using Status = int (*)(const PgConnection*);
        using Finish = void (*)(PgConnection*);
        using Exec = PgResult* (*)(PgConnection*, const char*);
        using ExecParams = PgResult* (*)(PgConnection*, const char*, int, const unsigned*,
                                         const char* const*, const int*, const int*, int);
        using Clear = void (*)(PgResult*);
        using ResultStatus = int (*)(const PgResult*);
        using ErrorField = char* (*)(const PgResult*, int);
        using Size = int (*)(const PgResult*);
        using IsNull = int (*)(const PgResult*, int, int);
        using Value = char* (*)(const PgResult*, int, int);
        using NoticeProcessor = void (*)(void*, const char*);
        using SetNoticeProcessor = NoticeProcessor (*)(PgConnection*, NoticeProcessor, void*);

        const meowlive::adapters::storage::postgres_detail::Driver driver_;
        const Connect connect_;
        const Status status_;
        std::unique_ptr<PgConnection, Finish> connection_;
        const Exec exec_;
        const ExecParams exec_params_;
        const Clear clear_;
        const ResultStatus result_status_;
        const ErrorField error_field_;
        const Size rows_;
        const Size columns_;
        const IsNull is_null_;
        const Value value_;
        const SetNoticeProcessor set_notice_processor_;
    };
}
