module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.llm;

import meowlive.standard;
import meowlive.domain.live_event;

export namespace meowlive::application
{
    struct ConversationTurn
    {
        std::string user;
        std::string assistant;
    };

    struct DecisionRequest
    {
        std::string persona;
        std::string system_prompt;
        std::string topic;
        std::vector<meowlive::domain::LiveEvent> events;
        std::vector<meowlive::application::ConversationTurn> history;
        std::vector<std::string> memory_context;
    };

    struct AgentDecision
    {
        std::vector<std::string> reply_to;
        std::optional<std::string> text;
        std::optional<std::string> topic;
    };

    struct ModelError
    {
        std::string message;
        bool retryable{};
    };

    struct ModelTool
    {
        std::string name;
        std::string description;
        std::string parameters_json;
    };

    struct ModelToolCall
    {
        std::string id;
        std::string name;
        std::string arguments_json;
    };

    struct ModelToolResult
    {
        std::string id;
        std::string name;
        std::string output_json;
        bool is_error{};
    };

    struct ModelOptions
    {
        std::vector<meowlive::application::ModelTool> tools;
        std::optional<std::string> continuation;
        std::vector<meowlive::application::ModelToolResult> results;
        bool final_turn{};
    };

    struct TokenUsage
    {
        std::uint64_t input{};
        std::uint64_t output{};
        std::uint64_t total{};
        std::optional<std::uint64_t> cache_read;
        std::optional<std::uint64_t> cache_write;
        std::optional<std::uint64_t> reasoning;
    };

    struct ModelTurn
    {
        std::optional<meowlive::application::AgentDecision> decision;
        std::vector<meowlive::application::ModelToolCall> tool_calls;
        std::optional<std::string> continuation;
        meowlive::application::TokenUsage usage;
        std::string finish_reason;
        std::optional<std::uint64_t> first_response_ms;
    };

    class LanguageModel
    {
    public:
        virtual ~LanguageModel() = default;

        virtual std::expected<meowlive::application::ModelTurn, meowlive::application::ModelError>
        turn(const meowlive::application::DecisionRequest& request,
             const meowlive::application::ModelOptions& options, std::stop_token stop) = 0;

        virtual std::expected<std::string, meowlive::application::ModelError>
            complete_json(std::string_view, std::string_view, std::stop_token);
    };
}

std::expected<std::string, meowlive::application::ModelError>
meowlive::application::LanguageModel::complete_json(std::string_view, std::string_view,
                                                    std::stop_token)
{
    return std::unexpected(meowlive::application::ModelError{"模型未提供结构化文本补全"});
}
