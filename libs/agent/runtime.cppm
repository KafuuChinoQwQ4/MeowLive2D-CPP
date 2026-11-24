export module meowlive.agent.runtime;

export namespace meowlive::agent
{
    enum class AgentPhase
    {
        Paused,
        Waiting,
        Deciding,
        Speaking,
    };
}
