module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.application.live_session;

import meowlive.standard;

import meowlive.application.live;
import meowlive.application.work_queue;
import meowlive.domain.live_event;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::application
{
    enum class LivePhase
    {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Disconnecting,
        Failed,
    };

    enum class LivePersistence
    {
        Stored,
        Duplicate,
        Failed,
    };

    enum class LiveAdmission
    {
        Accepted,
        Duplicate,
        Rejected,
    };

    struct LiveTime
    {
        std::uint64_t monotonic_ms{};
        std::uint64_t utc_ms{};
    };

    struct LiveRetry
    {
        std::chrono::milliseconds initial{1000};
        std::chrono::milliseconds max{30000};
    };

    struct LiveSessionCallbacks
    {
        std::function<meowlive::application::LiveTime()> clock;
        std::function<meowlive::application::LivePersistence(std::uint64_t session,
                                                             const meowlive::domain::LiveEvent&)>
            persist;
        std::function<meowlive::application::LiveAdmission(
            meowlive::domain::LiveEvent, std::uint64_t deadline, meowlive::application::LiveTime)>
            admit;
        std::function<void()> pause;
        std::function<void(std::string_view)> report;
    };

    struct LiveSnapshot
    {
        meowlive::application::LivePhase phase{};
        std::string room_id;
        unsigned accepted_events{};
        unsigned duplicate_events{};
        unsigned rejected_events{};
        unsigned reconnect_attempts{};
        std::string last_error;
    };

    class LiveSession
    {
    public:
        LiveSession(meowlive::application::LiveSource source,
                    meowlive::application::LiveSessionCallbacks callbacks,
                    std::size_t queue_capacity, meowlive::application::LiveRetry retry = {})
            : source_(std::move(source)), callbacks_(std::move(callbacks)), retry_(retry),
              events_(queue_capacity), worker_(
                                           [this](std::stop_token stop)
                                           {
                                               run(stop);
                                           })
        {
        }

        ~LiveSession()
        {
            close();
        }

        bool connect(meowlive::application::LiveCredentials credentials)
        {
            const std::lock_guard lock(mutex_);
            if (closed_ ||
                (snapshot_.phase != Phase::Disconnected && snapshot_.phase != Phase::Failed))
            {
                return false;
            }
            snapshot_ = {.phase = Phase::Connecting};
            credentials_ = std::move(credentials);
            source_stop_ = std::stop_source{};
            changed_.notify_one();
            return true;
        }

        void disconnect()
        {
            const std::lock_guard lock(mutex_);
            disconnect_locked();
        }

        void close()
        {
            {
                const std::lock_guard lock(mutex_);
                closed_ = true;
                disconnect_locked();
                events_.close();
                worker_.request_stop();
            }
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

        void reset()
        {
            const std::lock_guard lock(mutex_);
            if (snapshot_.phase == Phase::Disconnected || snapshot_.phase == Phase::Failed)
            {
                snapshot_ = {};
            }
        }

        bool connected() const noexcept
        {
            return connected_.load();
        }

        meowlive::application::LiveSnapshot snapshot() const
        {
            const std::lock_guard lock(mutex_);
            return snapshot_;
        }

    private:
        using Phase = meowlive::application::LivePhase;
        using Failure = meowlive::application::LiveSourceFailure;
        using Persistence = meowlive::application::LivePersistence;
        using Admission = meowlive::application::LiveAdmission;

        static void increment(unsigned& count)
        {
            if (count != std::numeric_limits<unsigned>::max())
            {
                ++count;
            }
        }

        void reject(std::string_view code)
        {
            increment(snapshot_.rejected_events);
            snapshot_.last_error = code;
            if (callbacks_.report)
            {
                callbacks_.report(code);
            }
        }

        void disconnect_locked()
        {
            if (snapshot_.phase == Phase::Connected || snapshot_.phase == Phase::Connecting ||
                snapshot_.phase == Phase::Reconnecting)
            {
                snapshot_.phase = Phase::Disconnecting;
                snapshot_.room_id.clear();
                connected_ = false;
                callbacks_.pause();
                source_stop_.request_stop();
            }
            changed_.notify_all();
        }

        bool admits(std::uint64_t session) const
        {
            return snapshot_.phase == Phase::Connected && session == session_;
        }

        void enqueue(std::uint64_t session, meowlive::domain::LiveEvent event)
        {
            const std::lock_guard lock(mutex_);
            if (!admits(session))
            {
                return;
            }

            const auto time = callbacks_.clock();
            const auto lifetime = meowlive::domain::response_lifetime_ms(event, time.utc_ms);
            if (lifetime == 0)
            {
                reject("live_event_expired");
                return;
            }
            const auto deadline = time.monotonic_ms + lifetime;
            if (events_.try_submit(
                    [this, session, deadline, event = std::move(event)]() mutable noexcept
                    {
                        try
                        {
                            store_and_admit(session, deadline, std::move(event));
                        }
                        catch (...)
                        {
                            const auto kind = meowlive::platform::log_current_exception(
                                "live.event.process", meowlive::platform::ExceptionDetails::Omit);
                            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
                            {
                                std::terminate();
                            }
                            const std::lock_guard lock(mutex_);
                            if (admits(session))
                            {
                                reject("live_event_internal_error");
                            }
                        }
                    }) == meowlive::application::WorkSubmission::Full)
            {
                reject("live_event_queue_full");
            }
        }

        void store_and_admit(std::uint64_t session, std::uint64_t deadline,
                             meowlive::domain::LiveEvent event)
        {
            {
                const std::lock_guard lock(mutex_);
                if (!is_fresh(session, deadline, callbacks_.clock()))
                {
                    return;
                }
            }

            const auto stored =
                callbacks_.persist ? callbacks_.persist(session, event) : Persistence::Stored;
            const std::lock_guard lock(mutex_);
            if (stored == Persistence::Failed)
            {
                if (admits(session))
                {
                    reject("live_event_storage_failed");
                }
                else if (callbacks_.report)
                {
                    callbacks_.report("live_event_storage_failed");
                }
                return;
            }
            const auto time = callbacks_.clock();
            if (!is_fresh(session, deadline, time))
            {
                return;
            }
            const auto result = stored == Persistence::Duplicate
                                    ? Admission::Duplicate
                                    : callbacks_.admit(std::move(event), deadline, time);
            switch (result)
            {
            case Admission::Accepted:
                increment(snapshot_.accepted_events);
                break;
            case Admission::Duplicate:
                increment(snapshot_.duplicate_events);
                break;
            case Admission::Rejected:
                reject("live_event_rejected");
                break;
            }
        }

        bool is_fresh(std::uint64_t session, std::uint64_t deadline,
                      meowlive::application::LiveTime time)
        {
            if (!admits(session))
            {
                return false;
            }
            if (deadline <= time.monotonic_ms)
            {
                reject("live_event_expired");
                return false;
            }
            return true;
        }

        void transport_lost()
        {
            if (snapshot_.phase == Phase::Connected || snapshot_.phase == Phase::Connecting)
            {
                snapshot_.phase = Phase::Reconnecting;
                snapshot_.room_id.clear();
                snapshot_.last_error = "live_disconnected";
                connected_ = false;
                callbacks_.pause();
            }
        }

        Failure run_source(const meowlive::application::LiveCredentials& credentials,
                           std::stop_token stop, std::uint64_t session, bool& received)
        {
            try
            {
                return source_(
                    credentials, stop,
                    [this, stop](std::string room_id)
                    {
                        const std::lock_guard lock(mutex_);
                        if (!stop.stop_requested())
                        {
                            snapshot_.phase = Phase::Connected;
                            snapshot_.room_id = std::move(room_id);
                            snapshot_.last_error.clear();
                            connected_ = true;
                        }
                    },
                    [this, session, &received](meowlive::domain::LiveEvent event)
                    {
                        received = true;
                        enqueue(session, std::move(event));
                    },
                    [this]
                    {
                        const std::lock_guard lock(mutex_);
                        transport_lost();
                    });
            }
            catch (...)
            {
                const auto kind = meowlive::platform::log_current_exception(
                    "live.source", meowlive::platform::ExceptionDetails::Omit);
                if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
                {
                    std::terminate();
                }
                return Failure::InternalError;
            }
        }

        void keep_connected(const meowlive::application::LiveCredentials& credentials,
                            std::stop_token stop, std::unique_lock<std::mutex>& lock)
        {
            auto delay = retry_.initial;
            auto failure = Failure::Stopped;
            while (!stop.stop_requested())
            {
                const auto session = ++session_;
                lock.unlock();
                bool received{};
                failure = run_source(credentials, stop, session, received);
                lock.lock();
                if (failure != Failure::Disconnected || stop.stop_requested())
                {
                    break;
                }
                transport_lost();
                if (received)
                {
                    delay = retry_.initial;
                }
                changed_.wait_for(lock, stop, delay,
                                  []
                                  {
                                      return false;
                                  });
                if (!stop.stop_requested())
                {
                    increment(snapshot_.reconnect_attempts);
                }
                delay += std::min(delay, retry_.max - delay);
            }
            transport_lost();
            snapshot_.room_id.clear();
            snapshot_.phase = Phase::Disconnected;
            snapshot_.last_error.clear();
            if (failure == Failure::InternalError)
            {
                snapshot_.phase = Phase::Failed;
                snapshot_.last_error = "live_source_internal_error";
            }
            else if (failure == Failure::Cleanup ||
                     (failure == Failure::Authorization && !stop.stop_requested()))
            {
                snapshot_.phase = Phase::Failed;
                snapshot_.last_error = failure == Failure::Cleanup ? "live_cleanup_failed"
                                                                   : "live_authorization_failed";
            }
        }

        void run(std::stop_token stop)
        try
        {
            std::unique_lock lock(mutex_);
            while (changed_.wait(lock, stop,
                                 [this]
                                 {
                                     return credentials_.has_value();
                                 }))
            {
                auto credentials = std::move(*credentials_);
                credentials_.reset();
                keep_connected(credentials, source_stop_.get_token(), lock);
            }
        }
        catch (...)
        {
            meowlive::platform::log_current_exception("live.session",
                                                      meowlive::platform::ExceptionDetails::Omit);
            std::terminate();
        }

        const meowlive::application::LiveSource source_;
        const meowlive::application::LiveSessionCallbacks callbacks_;
        const meowlive::application::LiveRetry retry_;
        mutable std::mutex mutex_;
        std::condition_variable_any changed_;
        meowlive::application::LiveSnapshot snapshot_;
        std::atomic<bool> connected_{};
        std::optional<meowlive::application::LiveCredentials> credentials_;
        std::stop_source source_stop_;
        std::uint64_t session_{};
        bool closed_{};
        meowlive::application::WorkQueue events_;
        std::jthread worker_;
    };
}
