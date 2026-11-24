export module meowlive.domain.speech;

export namespace meowlive::domain
{
    enum class SpeechStatus
    {
        Queued,
        Synthesizing,
        Ready,
        Playing,
        Completed,
        Cancelled,
        Failed,
        Unknown,
    };
}
