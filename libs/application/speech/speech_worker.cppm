module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.speech_worker;

import meowlive.standard;

import meowlive.application.scheduler;
import meowlive.application.speech;
import meowlive.application.reply_completion;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::application
{
    struct PlaybackLink
    {
        std::string bridge_id;
        std::function<bool(const meowlive::application::PcmAudio&)> supports_audio;
        std::function<void(const std::string&, std::uint32_t, meowlive::application::PcmAudio,
                           std::stop_token)>
            dispatch;
        std::function<void(std::uint32_t)> stop;
    };

    struct PlaybackReceipt
    {
        std::string utterance_id;
        std::uint32_t generation{};
        meowlive::application::PlaybackEvent status{};
        std::optional<std::string> error;
    };

    struct SpeechStatusSnapshot
    {
        std::string session_id;
        std::uint32_t generation{};
        bool connected{};
        std::vector<meowlive::application::SpeechTask> tasks;
    };

    struct SpeechVoices
    {
        std::string active;
        std::unordered_map<std::string, std::shared_ptr<meowlive::application::SpeechSynthesizer>>
            entries;
    };

    struct SpeechWorkerCallbacks
    {
        std::function<std::string()> new_id;
        std::function<void(const std::string&, const meowlive::application::PlaybackReceipt&)>
            record_receipt;
        std::function<void(const std::string&)> report_synthesis_failure;
        meowlive::application::ReserveReply reserve_reply;
    };

    class SpeechWorker
    {
    public:
        SpeechWorker(std::size_t capacity, std::size_t history_limit,
                     meowlive::application::SpeechVoices voices,
                     meowlive::application::SpeechWorkerCallbacks callbacks)
            : callbacks_(std::move(callbacks)), session_id_(callbacks_.new_id()),
              voices_(std::move(voices)), scheduler_(capacity, history_limit)
        {
            pending_replies_.reserve(capacity);
            worker_ = std::jthread(
                [this]
                {
                    run();
                });
        }

        ~SpeechWorker()
        {
            close();
        }

        const std::string& session_id() const
        {
            return session_id_;
        }

        std::uint32_t generation() const
        {
            const std::lock_guard lock(mutex_);
            return scheduler_.generation();
        }

        bool connected() const
        {
            const std::lock_guard lock(mutex_);
            return scheduler_.connected();
        }

        void reserve_bridge()
        {
            const std::lock_guard lock(mutex_);
            scheduler_.reserve();
        }

        void connect(meowlive::application::PlaybackLink link, std::uint32_t hello_generation)
        {
            const std::lock_guard lock(mutex_);
            if (hello_generation != scheduler_.generation())
            {
                link.stop(scheduler_.generation());
            }
            link_ = std::move(link);
            scheduler_.connect();
        }

        void disconnect()
        {
            const std::lock_guard lock(mutex_);
            disconnect_locked();
        }

        std::optional<std::string> resolve_voice(const std::string& requested) const
        {
            const std::lock_guard lock(mutex_);
            return resolve_voice_locked(requested);
        }

        bool freeze_for_update()
        {
            const std::lock_guard lock(mutex_);
            if (closed_ || updating_ || editing_voices_ || synthesizing_ || scheduler_.busy())
            {
                return false;
            }
            updating_ = true;
            return true;
        }

        void resume_after_update_check()
        {
            const std::lock_guard lock(mutex_);
            updating_ = false;
        }

        bool edit_voices(
            const std::function<std::optional<meowlive::application::SpeechVoices>()>& update)
        {
            {
                const std::lock_guard lock(mutex_);
                if (closed_ || updating_ || editing_voices_ || synthesizing_ || scheduler_.busy())
                {
                    return false;
                }
                editing_voices_ = true;
            }
            try
            {
                auto voices = update();
                const std::lock_guard lock(mutex_);
                if (voices)
                {
                    voices_ = std::move(*voices);
                }
                editing_voices_ = false;
            }
            catch (...)
            {
                const std::lock_guard lock(mutex_);
                editing_voices_ = false;
                throw;
            }
            return true;
        }

        std::expected<meowlive::application::SpeechTask, meowlive::application::EnqueueError>
        enqueue(std::string text, std::string voice_id)
        {
            const std::lock_guard lock(mutex_);
            auto result = enqueue_locked(callbacks_.new_id(), std::move(text), std::move(voice_id));
            if (result)
            {
                ready_.notify_one();
            }
            return result;
        }

        std::expected<meowlive::application::SpeechTask, std::string>
        enqueue_agent(std::string text, std::string voice, std::uint32_t generation,
                      std::vector<meowlive::application::ReplyEventKey> events = {})
        {
            const std::lock_guard lock(mutex_);
            if (generation != scheduler_.generation())
            {
                return std::unexpected("playback_generation_changed");
            }
            auto speech_id = callbacks_.new_id();
            std::unique_ptr<meowlive::application::ReplyReservation> reservation;
            if (callbacks_.reserve_reply && !events.empty())
            {
                auto reserved = callbacks_.reserve_reply(meowlive::application::ReplyCompletion{
                    speech_id, wall_ms(), 0, std::move(events)});
                if (!reserved)
                {
                    using Error = meowlive::application::ReplyReservationError;
                    switch (reserved.error())
                    {
                    case Error::Full:
                        return std::unexpected("reply_completion_outbox_full");
                    case Error::Closed:
                        return std::unexpected("reply_completion_outbox_closed");
                    case Error::Invalid:
                        return std::unexpected("reply_completion_invalid");
                    }
                }
                reservation = std::move(*reserved);
            }
            auto task = enqueue_locked(speech_id, std::move(text), std::move(voice));
            if (!task)
            {
                using Error = meowlive::application::EnqueueError;
                switch (task.error())
                {
                case Error::Full:
                    return std::unexpected("queue_full");
                case Error::Disconnected:
                    return std::unexpected("bridge_disconnected");
                case Error::VoiceUnavailable:
                    return std::unexpected("unknown_voice");
                case Error::VoiceEditInProgress:
                    return std::unexpected("voice_edit_in_progress");
                }
            }
            scheduler_.track(*task);
            if (reservation)
            {
                pending_replies_.push_back({std::move(speech_id), std::move(reservation)});
            }
            ready_.notify_one();
            return *task;
        }

        std::optional<meowlive::application::SpeechTask> agent_playback() const
        {
            const std::lock_guard lock(mutex_);
            return scheduler_.tracked();
        }

        meowlive::application::SpeechStatusSnapshot status() const
        {
            const std::lock_guard lock(mutex_);
            return snapshot();
        }

        meowlive::application::SpeechStatusSnapshot stop()
        {
            const std::lock_guard lock(mutex_);
            scheduler_.stop();
            pending_replies_.clear();
            cancel_current();
            if (link_)
            {
                link_->stop(scheduler_.generation());
            }
            return snapshot();
        }

        void receipt(std::string_view bridge_id,
                     const meowlive::application::PlaybackReceipt& receipt)
        {
            const std::lock_guard lock(mutex_);
            if (!link_ || link_->bridge_id != bridge_id ||
                !scheduler_.receive(receipt.utterance_id, receipt.generation, receipt.status,
                                    receipt.error))
            {
                return;
            }
            if (receipt.status != meowlive::application::PlaybackEvent::Started)
            {
                cancel_current();
                finish_reply(receipt.utterance_id,
                             receipt.status == meowlive::application::PlaybackEvent::Completed);
                callbacks_.record_receipt(session_id_, receipt);
                ready_.notify_one();
            }
        }

        void close()
        {
            {
                const std::lock_guard lock(mutex_);
                closed_ = true;
                disconnect_locked();
            }
            ready_.notify_one();
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

    private:
        struct PendingReply
        {
            std::string speech_id;
            std::unique_ptr<meowlive::application::ReplyReservation> reservation;
        };

        void finish_reply(std::string_view id, bool completed)
        {
            const auto reply = std::ranges::find(pending_replies_, id, &PendingReply::speech_id);
            if (reply == pending_replies_.end())
            {
                return;
            }
            if (completed)
            {
                reply->reservation->confirm(wall_ms());
            }
            pending_replies_.erase(reply);
        }

        static std::uint64_t wall_ms()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        std::optional<std::string> resolve_voice_locked(const std::string& requested) const
        {
            const auto& id = requested == "active" ? voices_.active : requested;
            return voices_.entries.contains(id) ? std::optional(id) : std::nullopt;
        }

        std::expected<meowlive::application::SpeechTask, meowlive::application::EnqueueError>
        enqueue_locked(std::string id, std::string text, std::string voice_id)
        {
            if (editing_voices_ || updating_)
            {
                return std::unexpected(meowlive::application::EnqueueError::VoiceEditInProgress);
            }
            auto resolved = resolve_voice_locked(voice_id);
            if (!resolved)
            {
                return std::unexpected(meowlive::application::EnqueueError::VoiceUnavailable);
            }
            return scheduler_.enqueue(std::move(id), std::move(text), std::move(*resolved));
        }

        meowlive::application::SpeechStatusSnapshot snapshot() const
        {
            return {session_id_,
                    scheduler_.generation(),
                    scheduler_.connected(),
                    {scheduler_.tasks().begin(), scheduler_.tasks().end()}};
        }

        void cancel_current()
        {
            current_.request_stop();
        }

        void disconnect_locked()
        {
            scheduler_.disconnect();
            pending_replies_.clear();
            cancel_current();
            link_.reset();
        }

        std::expected<meowlive::application::PcmAudio, std::string>
        synthesize(const meowlive::application::SpeechTask& task,
                   meowlive::application::SpeechSynthesizer& voice, std::stop_token stop)
        {
            try
            {
                return voice.synthesize({task.text, task.voice_id}, stop);
            }
            catch (...)
            {
                const auto kind = meowlive::platform::log_current_exception(
                    "speech.synthesize", meowlive::platform::ExceptionDetails::Omit);
                if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
                {
                    std::terminate();
                }
                return std::unexpected("speech_synthesis_internal_error");
            }
        }

        void run()
        try
        {
            std::unique_lock lock(mutex_);
            for (;;)
            {
                ready_.wait(lock,
                            [this]
                            {
                                return closed_ || scheduler_.has_work();
                            });
                if (closed_)
                {
                    return;
                }
                const auto task = scheduler_.claim();
                const auto voice = voices_.entries.at(task.voice_id);
                synthesizing_ = true;
                current_ = std::stop_source{};
                auto token = current_.get_token();
                lock.unlock();
                auto audio = synthesize(task, *voice, token);
                lock.lock();
                synthesizing_ = false;
                if (token.stop_requested())
                {
                    continue;
                }
                if (!audio)
                {
                    scheduler_.fail_synthesis(task.id, task.generation, std::move(audio.error()));
                    finish_reply(task.id, false);
                    callbacks_.report_synthesis_failure(task.id);
                    continue;
                }
                if (audio->channels == 0 || audio->samples.empty() ||
                    audio->samples.size() % audio->channels != 0 ||
                    (link_ && !link_->supports_audio(*audio)))
                {
                    scheduler_.fail_synthesis(task.id, task.generation, "invalid synthesized PCM");
                    finish_reply(task.id, false);
                    continue;
                }
                if (link_ && scheduler_.dispatch(task.id, task.generation))
                {
                    link_->dispatch(task.id, task.generation, std::move(*audio), token);
                }
            }
        }
        catch (...)
        {
            meowlive::platform::log_current_exception("speech.worker",
                                                      meowlive::platform::ExceptionDetails::Omit);
            std::terminate();
        }

        const meowlive::application::SpeechWorkerCallbacks callbacks_;
        const std::string session_id_;
        meowlive::application::SpeechVoices voices_;
        mutable std::mutex mutex_;
        std::condition_variable ready_;
        meowlive::application::Scheduler scheduler_;
        std::optional<meowlive::application::PlaybackLink> link_;
        std::vector<PendingReply> pending_replies_;
        std::stop_source current_;
        bool closed_{};
        bool editing_voices_{};
        bool updating_{};
        bool synthesizing_{};
        std::jthread worker_;
    };
}
