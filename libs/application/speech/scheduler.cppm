module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.scheduler;

import meowlive.standard;

import meowlive.domain.speech;

namespace
{
    using Status = meowlive::domain::SpeechStatus;

    bool terminal(Status status)
    {
        return status == Status::Completed || status == Status::Cancelled ||
               status == Status::Failed || status == Status::Unknown;
    }
}

export namespace meowlive::application
{
    struct SpeechTask
    {
        std::string id;
        std::uint32_t generation{};
        std::string text;
        std::string voice_id;
        meowlive::domain::SpeechStatus status{meowlive::domain::SpeechStatus::Queued};
        std::optional<std::string> error;
    };

    enum class EnqueueError
    {
        Disconnected,
        Full,
        VoiceUnavailable,
        VoiceEditInProgress,
    };

    enum class PlaybackEvent
    {
        Started,
        Completed,
        Cancelled,
        Failed,
    };

    class Scheduler
    {
    public:
        Scheduler(std::size_t capacity, std::size_t history_limit)
            : capacity_(capacity), history_limit_(history_limit)
        {
        }

        std::uint32_t generation() const
        {
            return generation_;
        }
        bool connected() const
        {
            return connected_;
        }
        const std::deque<meowlive::application::SpeechTask>& tasks() const
        {
            return tasks_;
        }

        void reserve()
        {
            ++generation_;
        }
        void connect()
        {
            connected_ = true;
        }

        std::expected<meowlive::application::SpeechTask, meowlive::application::EnqueueError>
        enqueue(std::string id, std::string text, std::string voice_id)
        {
            if (!connected_)
            {
                return std::unexpected(meowlive::application::EnqueueError::Disconnected);
            }
            if (unfinished_ == capacity_)
            {
                return std::unexpected(meowlive::application::EnqueueError::Full);
            }
            tasks_.push_back({std::move(id),
                              generation_,
                              std::move(text),
                              std::move(voice_id),
                              Status::Queued,
                              {}});
            ++unfinished_;
            return tasks_.back();
        }

        void track(const meowlive::application::SpeechTask& task)
        {
            tracked_ = task;
        }

        const std::optional<meowlive::application::SpeechTask>& tracked() const
        {
            return tracked_;
        }

        bool has_work() const
        {
            return connected_ && !current_ && unfinished_;
        }

        bool busy() const
        {
            return unfinished_ != 0;
        }

        meowlive::application::SpeechTask claim()
        {
            const auto queued = std::ranges::find(tasks_, Status::Queued,
                                                  &meowlive::application::SpeechTask::status);
            current_ = queued->id;
            queued->status = Status::Synthesizing;
            update_tracked(*queued);
            return *queued;
        }

        bool dispatch(std::string_view id, std::uint32_t generation)
        {
            const auto task = current(id, generation);
            if (task == tasks_.end() || task->status != Status::Synthesizing)
            {
                return false;
            }
            task->status = Status::Ready;
            update_tracked(*task);
            return true;
        }

        bool fail_synthesis(std::string_view id, std::uint32_t generation, std::string error)
        {
            const auto task = current(id, generation);
            if (task == tasks_.end() || task->status != Status::Synthesizing)
            {
                return false;
            }
            finish(*task, Status::Failed, std::move(error));
            return true;
        }

        bool receive(std::string_view id, std::uint32_t generation,
                     meowlive::application::PlaybackEvent event, std::optional<std::string> error)
        {
            const auto task = current(id, generation);
            if (task == tasks_.end() ||
                (task->status != Status::Ready && task->status != Status::Playing))
            {
                return false;
            }
            switch (event)
            {
            case meowlive::application::PlaybackEvent::Started:
                if (task->status != Status::Ready)
                {
                    return false;
                }
                task->status = Status::Playing;
                update_tracked(*task);
                break;
            case meowlive::application::PlaybackEvent::Completed:
                if (task->status != Status::Playing)
                {
                    return false;
                }
                finish(*task, Status::Completed, {});
                break;
            case meowlive::application::PlaybackEvent::Cancelled:
                finish(*task, Status::Cancelled, {});
                break;
            case meowlive::application::PlaybackEvent::Failed:
                finish(*task, Status::Failed, std::move(error));
                break;
            }
            return true;
        }

        void stop()
        {
            cancel_all(false);
        }

        void disconnect()
        {
            if (connected_)
            {
                connected_ = false;
                cancel_all(true);
            }
        }

    private:
        std::deque<meowlive::application::SpeechTask>::iterator current(std::string_view id,
                                                                        std::uint32_t generation)
        {
            if (!current_ || *current_ != id || generation != generation_)
            {
                return tasks_.end();
            }
            return std::ranges::find(tasks_, id, &meowlive::application::SpeechTask::id);
        }

        void finish(meowlive::application::SpeechTask& task, Status status,
                    std::optional<std::string> error)
        {
            task.status = status;
            task.error = std::move(error);
            update_tracked(task);
            current_.reset();
            --unfinished_;
            prune();
        }

        void cancel_all(bool disconnected)
        {
            ++generation_;
            for (auto& task : tasks_)
            {
                if (!terminal(task.status))
                {
                    const bool dispatched =
                        task.status == Status::Ready || task.status == Status::Playing;
                    task.status = disconnected && dispatched ? Status::Unknown : Status::Cancelled;
                    update_tracked(task);
                }
            }
            current_.reset();
            unfinished_ = 0;
            prune();
        }

        void update_tracked(const meowlive::application::SpeechTask& task)
        {
            if (tracked_ && tracked_->id == task.id)
            {
                tracked_ = task;
            }
        }

        void prune()
        {
            auto excess = tasks_.size() - unfinished_;
            if (excess <= history_limit_)
            {
                return;
            }
            excess -= history_limit_;
            std::erase_if(tasks_,
                          [&excess](const meowlive::application::SpeechTask& task)
                          {
                              if (excess && terminal(task.status))
                              {
                                  --excess;
                                  return true;
                              }
                              return false;
                          });
        }

        const std::size_t capacity_;
        const std::size_t history_limit_;
        std::deque<meowlive::application::SpeechTask> tasks_;
        std::optional<std::string> current_;
        std::optional<meowlive::application::SpeechTask> tracked_;
        std::size_t unfinished_{};
        std::uint32_t generation_{};
        bool connected_{};
    };
}
