module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.execution;

import meowlive.standard;

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
