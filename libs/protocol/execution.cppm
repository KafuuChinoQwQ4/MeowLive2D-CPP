module;

#if defined(_WIN32)
#include <cstdint>
#include <optional>
#include <string>
#endif

export module meowlive.protocol.execution;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::protocol
{
    enum class ExecutionStatus
    {
        Started,
        Completed,
        Cancelled,
        Failed,
    };

    struct ExecutionReceipt
    {
        std::string utterance_id;
        std::uint32_t generation{};
        meowlive::protocol::ExecutionStatus status{};
        std::optional<std::string> error;

        bool operator==(const meowlive::protocol::ExecutionReceipt&) const = default;
    };
}
