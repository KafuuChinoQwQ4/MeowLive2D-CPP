#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.application.reply_completion;
import meowlive.application.scheduler;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.domain.speech;

namespace
{
    namespace playback = meowlive::application;

    void require(bool passed, std::string_view message)
    {
        if (!passed)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    class Synthesizer final : public playback::SpeechSynthesizer
    {
    public:
        std::expected<playback::PcmAudio, std::string> synthesize(const playback::SynthesisRequest&,
                                                                  std::stop_token) override
        {
            return playback::PcmAudio{24000, 1, {1, 2, 3}};
        }
    };

    class ReservedReply final : public playback::ReplyReservation
    {
    public:
        ReservedReply(playback::ReplyCompletion completion,
                      std::vector<playback::ReplyCompletion>& completed, unsigned& cancelled)
            : completion_(std::move(completion)), completed_(completed), cancelled_(cancelled)
        {
        }

        ~ReservedReply() override
        {
            if (completion_.completed_at_ms == 0)
            {
                ++cancelled_;
            }
        }

        void confirm(std::uint64_t completed_at_ms) noexcept override
        {
            completion_.completed_at_ms = completed_at_ms;
            completed_.push_back(std::move(completion_));
        }

    private:
        playback::ReplyCompletion completion_;
        std::vector<playback::ReplyCompletion>& completed_;
        unsigned& cancelled_;
    };

    void check_confirmed_completions()
    {
        unsigned id{};
        std::binary_semaphore dispatched(0);
        std::vector<playback::ReplyCompletion> completed;
        completed.reserve(4);
        unsigned cancelled{};
        bool storage_full{};
        playback::SpeechVoices voices;
        voices.active = "fixture";
        voices.entries["fixture"] = std::make_shared<Synthesizer>();
        playback::SpeechWorker speech(
            2, 4, std::move(voices),
            {.new_id =
                 [&]
             {
                 return std::to_string(++id);
             },
             .record_receipt =
                 [](const std::string&, const playback::PlaybackReceipt&)
             {
             },
             .report_synthesis_failure =
                 [](const std::string&)
             {
             },
             .reserve_reply =
                 [&](playback::ReplyCompletion completion) -> playback::ReplyReserveResult
             {
                 if (storage_full)
                 {
                     return std::unexpected(playback::ReplyReservationError::Full);
                 }
                 return std::make_unique<ReservedReply>(std::move(completion), completed,
                                                        cancelled);
             }});
        speech.connect(
            {.bridge_id = "fixture-device",
             .supports_audio =
                 [](const playback::PcmAudio&)
             {
                 return true;
             },
             .dispatch =
                 [&](const std::string&, std::uint32_t, playback::PcmAudio, std::stop_token)
             {
                 dispatched.release();
             },
             .stop =
                 [](std::uint32_t)
             {
             }},
            speech.generation());

        const auto request = [&](std::string event)
        {
            auto task = speech.enqueue_agent("fixture", "active", speech.generation(),
                                             {{"bilibili", std::move(event)}});
            require(task.has_value() && dispatched.try_acquire_for(std::chrono::seconds(3)),
                    "Fixture Agent speech did not dispatch");
            return *task;
        };
        const auto send = [&](const playback::SpeechTask& task, playback::PlaybackEvent status)
        {
            speech.receipt("fixture-device", {task.id, task.generation, status, {}});
        };

        const auto first = request("completed-before-stop");
        send(first, playback::PlaybackEvent::Completed);
        require(completed.empty(), "A completion before playback started reached storage");
        send(first, playback::PlaybackEvent::Started);
        send(first, playback::PlaybackEvent::Completed);
        send(first, playback::PlaybackEvent::Completed);
        speech.stop();
        require(completed.size() == 1 && completed.front().speech_id == first.id &&
                    completed.front().completed_at_ms > 0 && cancelled == 0 &&
                    completed.front().events.size() == 1 &&
                    completed.front().events.front().id == "completed-before-stop" &&
                    speech.agent_playback()->status == meowlive::domain::SpeechStatus::Completed,
                "Completion was not preserved or a duplicate reached storage");

        const auto second = request("stopped-before-completed");
        send(second, playback::PlaybackEvent::Started);
        speech.stop();
        send(second, playback::PlaybackEvent::Completed);
        require(completed.size() == 1 && cancelled == 1 &&
                    speech.agent_playback()->status == meowlive::domain::SpeechStatus::Cancelled,
                "Stopped speech was settled by a late completion");

        const auto third = request("disconnected-before-completed");
        send(third, playback::PlaybackEvent::Started);
        speech.disconnect();
        send(third, playback::PlaybackEvent::Completed);
        require(completed.size() == 1 && cancelled == 2 &&
                    speech.agent_playback()->status == meowlive::domain::SpeechStatus::Unknown,
                "Disconnected speech was settled by a late completion");

        storage_full = true;
        const auto refused = speech.enqueue_agent("fixture", "active", speech.generation(),
                                                  {{"bilibili", "outbox-full"}});
        require(!refused && refused.error() == "reply_completion_outbox_full",
                "An Agent reply could be accepted without a slot to record its completion");
    }
}

int main()
{
    try
    {
        check_confirmed_completions();
        std::println("Reply settlement follows the scheduler's confirmed playback result.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Reply confirmation failed: {}", error.what());
        return 1;
    }
}
