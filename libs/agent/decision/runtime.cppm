module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.agent.runtime;

import meowlive.standard;

import meowlive.application.llm;

export namespace meowlive::agent
{
    enum class AgentPhase
    {
        Paused,
        Waiting,
        Deciding,
        Speaking,
    };

    struct ToolLoop
    {
        std::vector<meowlive::application::ModelTool> tools;
        unsigned rounds{2};
        unsigned calls_per_round{4};
        unsigned retries{1};
        std::function<meowlive::application::ModelToolResult(
            const meowlive::application::ModelToolCall&, std::stop_token)>
            invoke;
        std::function<void(const std::expected<meowlive::application::ModelTurn,
                                               meowlive::application::ModelError>&,
                           std::chrono::system_clock::time_point, bool)>
            observed;
    };

    std::expected<meowlive::application::AgentDecision, std::string>
    decide(meowlive::application::LanguageModel& model,
           const meowlive::application::DecisionRequest& request,
           const meowlive::agent::ToolLoop& loop, std::stop_token stop)
    {
        meowlive::application::ModelOptions options;
        for (unsigned round = 0; round <= loop.rounds && !stop.stop_requested(); ++round)
        {
            options.final_turn = round == loop.rounds || loop.tools.empty();
            options.tools =
                options.final_turn ? std::vector<meowlive::application::ModelTool>{} : loop.tools;
            const auto attempt = [&]
            {
                const auto started = std::chrono::system_clock::now();
                auto result = model.turn(request, options, stop);
                loop.observed(result, started, stop.stop_requested());
                return result;
            };
            auto turn = attempt();
            for (unsigned retry = 0;
                 !turn && turn.error().retryable && retry < loop.retries && !stop.stop_requested();
                 ++retry)
            {
                turn = attempt();
            }
            if (stop.stop_requested())
            {
                break;
            }
            if (!turn)
            {
                return std::unexpected(turn.error().message);
            }
            if (turn->tool_calls.empty() && turn->decision)
            {
                return std::move(*turn->decision);
            }
            if (options.final_turn || turn->tool_calls.empty() || !turn->continuation ||
                turn->tool_calls.size() > loop.calls_per_round)
            {
                return std::unexpected("invalid_model_turn");
            }
            options.continuation = std::move(turn->continuation);
            options.results.clear();
            for (const auto& call : turn->tool_calls)
            {
                if (stop.stop_requested())
                {
                    break;
                }
                options.results.push_back(loop.invoke(call, stop));
            }
        }
        return std::unexpected("decision_cancelled");
    }
}
