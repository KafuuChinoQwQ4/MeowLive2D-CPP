module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.training_worker;

import meowlive.standard;
import meowlive.application.voice_models;
import meowlive.adapters.speech.model_weights;
import meowlive.server.training_jobs;

export namespace meowlive::server
{
    struct Transcription
    {
        std::string text;
        std::string language;
        std::optional<meowlive::application::VoiceModelSelection> model;
    };

    enum class TranscriptionFailureKind
    {
        OperationFailed,
        OutOfMemory,
        InternalError,
    };

    struct TranscriptionFailure
    {
        meowlive::server::TranscriptionFailureKind kind;
        std::string message;
    };

    class TrainingWorker
    {
    public:
        using Result =
            std::expected<meowlive::server::Transcription, meowlive::server::TranscriptionFailure>;
        using Reply = std::function<void(meowlive::server::TrainingWorker::Result)>;

        TrainingWorker(const std::filesystem::path& data,
                       const std::filesystem::path& executable_directory,
                       const std::string& speech_url);
        ~TrainingWorker();

        bool configured() const;
        bool transcribe(std::string request_id, std::string audio, std::string language,
                        std::optional<meowlive::application::InstalledVoiceModel> model,
                        meowlive::server::TrainingWorker::Reply reply);
        void close();

        meowlive::application::VoiceModelLibrary& voice_models() noexcept;
        meowlive::server::TrainingJobs& jobs() noexcept;
        std::shared_ptr<meowlive::adapters::ManagedModelWeights> model_control() const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
