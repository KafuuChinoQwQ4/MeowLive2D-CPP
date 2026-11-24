module;

#if defined(_WIN32)
#include <optional>
#include <string>
#include <vector>
#endif

export module meowlive.application.llm;

#if !defined(_WIN32)
import std;
#endif
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
}
