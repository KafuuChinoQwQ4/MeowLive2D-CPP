#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.application.live;
import meowlive.application.live_session;
import meowlive.domain.live_event;

namespace
{
    namespace live = meowlive::application;
    namespace domain = meowlive::domain;

    constexpr std::uint64_t utc_ms = 1'800'000'000'000;
    constexpr std::uint64_t monotonic_ms = 500'000;
    constexpr auto test_timeout = std::chrono::seconds(3);

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    template <typename Probe> void wait_until(Probe probe, std::string_view message)
    {
        const auto deadline = std::chrono::steady_clock::now() + test_timeout;
        while (!probe())
        {
            require(std::chrono::steady_clock::now() < deadline, message);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void wait_until_stopped(std::stop_token stop)
    {
        std::mutex mutex;
        std::condition_variable_any stopped;
        std::unique_lock lock(mutex);
        stopped.wait(lock, stop,
                     []
                     {
                         return false;
                     });
    }

    live::LiveCredentials credentials()
    {
        return {.app_id = 1,
                .access_key_id = "fixture-id",
                .access_key_secret = "fixture-secret",
                .identity_code = "fixture-anchor"};
    }

    domain::LiveEvent event(std::string_view id, std::uint64_t timestamp, domain::EventKind kind)
    {
        return {.id = std::string(id),
                .source = "bilibili",
                .viewer = "Fixture viewer",
                .occurred_at_ms = timestamp,
                .kind = std::move(kind)};
    }

    live::LiveTime fixed_time()
    {
        return {.monotonic_ms = monotonic_ms, .utc_ms = utc_ms};
    }

    struct Admission
    {
        std::string id;
        std::uint64_t occurred_at_ms;
        std::uint64_t monotonic_deadline;

        bool operator==(const Admission&) const = default;
    };

    std::vector<Admission> check_ttl(bool persist)
    {
        std::atomic<unsigned> writes{};
        std::vector<Admission> admitted;
        live::LiveSessionCallbacks callbacks{
            .clock = fixed_time,
            .admit =
                [&](domain::LiveEvent admitted_event, std::uint64_t deadline, live::LiveTime)
            {
                admitted.push_back(
                    {std::move(admitted_event.id), admitted_event.occurred_at_ms, deadline});
                return live::LiveAdmission::Accepted;
            },
            .pause =
                []
            {
            },
        };
        if (persist)
        {
            callbacks.persist = [&](std::uint64_t, const domain::LiveEvent&)
            {
                ++writes;
                return live::LivePersistence::Stored;
            };
        }

        const live::LiveSource source = [](const auto&, std::stop_token stop, const auto& connected,
                                           const auto& receive, const auto&)
        {
            connected("42");
            receive(event("chat", utc_ms - 59'000, domain::Chat{"fresh"}));
            receive(event("chat-expired", utc_ms - 60'000, domain::Chat{"old"}));
            receive(event("gift", utc_ms - 58'000, domain::Gift{"a gift", 1}));
            receive(event("gift-expired", utc_ms - 61'000, domain::Gift{"an old gift", 1}));
            receive(event("enter", utc_ms - 57'000, domain::RoomEnter{}));
            receive(event("enter-expired", utc_ms - 60'000, domain::RoomEnter{}));
            receive(
                event("sc", utc_ms - 61'000,
                      domain::SuperChat{"still displayed", 30, utc_ms - 61'000, utc_ms + 39'000}));
            receive(event("sc-expired", utc_ms - 1'000,
                          domain::SuperChat{"ended", 30, utc_ms - 1'000, utc_ms}));
            wait_until_stopped(stop);
            return live::LiveSourceFailure::Stopped;
        };

        live::LiveSession session(source, std::move(callbacks), 16);
        require(session.connect(credentials()), "Fixed-clock live fixture did not start");
        wait_until(
            [&]
            {
                const auto snapshot = session.snapshot();
                return snapshot.accepted_events == 4 && snapshot.rejected_events == 4;
            },
            "Expected four fresh and four expired live events");
        require(!persist || writes >= 4, "Fresh live events bypassed the configured persistence");
        session.disconnect();
        return admitted;
    }

    void check_storage_expiry()
    {
        std::binary_semaphore writing(0);
        std::binary_semaphore finish_write(0);
        std::mutex clock_mutex;
        auto time = fixed_time();
        const live::LiveSource source = [](const auto&, std::stop_token stop, const auto& connected,
                                           const auto& receive, const auto&)
        {
            connected("42");
            receive(event("expires-in-storage", utc_ms - 59'000, domain::Chat{"fresh initially"}));
            wait_until_stopped(stop);
            return live::LiveSourceFailure::Stopped;
        };
        live::LiveSession session(source,
                                  {.clock =
                                       [&]
                                   {
                                       const std::lock_guard lock(clock_mutex);
                                       return time;
                                   },
                                   .persist =
                                       [&](std::uint64_t, const domain::LiveEvent&)
                                   {
                                       writing.release();
                                       return finish_write.try_acquire_for(test_timeout)
                                                  ? live::LivePersistence::Stored
                                                  : live::LivePersistence::Failed;
                                   },
                                   .admit =
                                       [](domain::LiveEvent, std::uint64_t, live::LiveTime)
                                   {
                                       return live::LiveAdmission::Accepted;
                                   },
                                   .pause =
                                       []
                                   {
                                   }},
                                  2);
        require(session.connect(credentials()), "Storage expiry source did not start");
        require(writing.try_acquire_for(test_timeout), "Fresh event never entered storage");
        {
            const std::lock_guard lock(clock_mutex);
            time.monotonic_ms += 1'000;
            time.utc_ms -= 10'000;
        }
        finish_write.release();
        wait_until(
            [&]
            {
                return session.snapshot().rejected_events == 1;
            },
            "Persistence latency did not expire the event after a wall clock rollback");
        require(session.snapshot().accepted_events == 0, "Expired event reached the Agent");
    }

    void check_storage_results()
    {
        std::atomic<unsigned> admissions{};
        const live::LiveSource source = [](const auto&, std::stop_token stop, const auto& connected,
                                           const auto& receive, const auto&)
        {
            connected("42");
            receive(event("duplicate", utc_ms, domain::Chat{"already stored"}));
            receive(event("failed", utc_ms, domain::Chat{"storage unavailable"}));
            receive(event("stored", utc_ms, domain::Chat{"ready"}));
            wait_until_stopped(stop);
            return live::LiveSourceFailure::Stopped;
        };
        live::LiveSession session(source,
                                  {.clock = fixed_time,
                                   .persist =
                                       [](std::uint64_t, const domain::LiveEvent& input)
                                   {
                                       if (input.id == "duplicate")
                                       {
                                           return live::LivePersistence::Duplicate;
                                       }
                                       if (input.id == "failed")
                                       {
                                           return live::LivePersistence::Failed;
                                       }
                                       return live::LivePersistence::Stored;
                                   },
                                   .admit =
                                       [&](domain::LiveEvent, std::uint64_t, live::LiveTime)
                                   {
                                       ++admissions;
                                       return live::LiveAdmission::Accepted;
                                   },
                                   .pause =
                                       []
                                   {
                                   }},
                                  3);

        require(session.connect(credentials()), "Storage result fixture did not start");
        wait_until(
            [&]
            {
                return session.snapshot().accepted_events == 1;
            },
            "Live events could not reach the Agent after a storage failure");
        const auto snapshot = session.snapshot();
        require(admissions == 1 && snapshot.duplicate_events == 1 &&
                    snapshot.rejected_events == 1 &&
                    snapshot.last_error == "live_event_storage_failed",
                "Storage failure or duplicate was not reported or reached the Agent");
    }

    void check_disconnect_during_storage()
    {
        std::binary_semaphore write_started(0);
        std::binary_semaphore finish_write(0);
        std::binary_semaphore advance_fixture(0);
        std::binary_semaphore heartbeat(0);
        std::atomic<unsigned> stopped{};
        std::atomic<unsigned> pauses{};
        unsigned admitted{};

        const live::LiveSource source = [&](const auto&, std::stop_token stop,
                                            const auto& connected, const auto& receive, const auto&)
        {
            connected("42");
            receive(event("late-write", utc_ms, domain::Chat{"in storage"}));
            if (advance_fixture.try_acquire_for(test_timeout))
            {
                receive(event("overflow", utc_ms, domain::Chat{"queue full"}));
                heartbeat.release();
            }
            wait_until_stopped(stop);
            ++stopped;
            return live::LiveSourceFailure::Stopped;
        };

        {
            live::LiveSession session(source,
                                      {.clock = fixed_time,
                                       .persist =
                                           [&](std::uint64_t, const domain::LiveEvent&)
                                       {
                                           write_started.release();
                                           return finish_write.try_acquire_for(test_timeout)
                                                      ? live::LivePersistence::Stored
                                                      : live::LivePersistence::Failed;
                                       },
                                       .admit =
                                           [&](domain::LiveEvent, std::uint64_t, live::LiveTime)
                                       {
                                           ++admitted;
                                           return live::LiveAdmission::Accepted;
                                       },
                                       .pause =
                                           [&]
                                       {
                                           ++pauses;
                                       }},
                                      1);

            require(session.connect(credentials()), "Storage delay fixture did not start");
            require(write_started.try_acquire_for(test_timeout),
                    "Storage did not receive a live event");
            advance_fixture.release();
            require(heartbeat.try_acquire_for(test_timeout),
                    "Storage delay blocked the live source heartbeat");
            require(session.snapshot().rejected_events == 1,
                    "Overflow was not counted as rejected");
            session.disconnect();
            wait_until(
                [&]
                {
                    return session.snapshot().phase == live::LivePhase::Disconnected &&
                           stopped == 1;
                },
                "Disconnect could not clean up the source during a storage wait");
            require(pauses > 0 && session.snapshot().room_id.empty(),
                    "Manual disconnect did not pause the Agent or clear the room");

            finish_write.release();
        }
        require(admitted == 0, "An old session event reached the Agent after disconnect");
    }

    void check_retry_and_authorization_failure()
    {
        std::atomic<unsigned> attempts{};
        std::atomic<unsigned> pauses{};
        std::binary_semaphore cleanup_started(0);
        std::binary_semaphore finish_cleanup(0);
        const live::LiveSource source = [&](const auto&, std::stop_token stop,
                                            const auto& connected, const auto& receive,
                                            const auto& lost)
        {
            const auto attempt = ++attempts;
            if (attempt == 1)
            {
                connected("42");
                lost();
                cleanup_started.release();
                finish_cleanup.try_acquire_for(test_timeout);
                return live::LiveSourceFailure::Disconnected;
            }
            if (attempt == 2)
            {
                return live::LiveSourceFailure::Authorization;
            }
            connected("43");
            receive(event("resumed", utc_ms, domain::Chat{"ready"}));
            wait_until_stopped(stop);
            return live::LiveSourceFailure::Stopped;
        };
        live::LiveSession session(
            source,
            {.clock = fixed_time,
             .admit =
                 [](domain::LiveEvent, std::uint64_t, live::LiveTime)
             {
                 return live::LiveAdmission::Accepted;
             },
             .pause =
                 [&]
             {
                 ++pauses;
             }},
            1, {.initial = std::chrono::milliseconds(10), .max = std::chrono::milliseconds(20)});

        require(session.connect(credentials()), "Retry fixture did not start");
        require(cleanup_started.try_acquire_for(test_timeout), "Controlled source did not drop");
        require(pauses > 0 && session.snapshot().room_id.empty() && attempts == 1,
                "Source loss did not pause the Agent before cleanup");
        finish_cleanup.release();
        wait_until(
            [&]
            {
                return session.snapshot().phase == live::LivePhase::Failed;
            },
            "Retry did not end with an authorization failure");
        const auto snapshot = session.snapshot();
        require(attempts == 2 && snapshot.reconnect_attempts == 1 && !snapshot.last_error.empty(),
                "Authorization failure was not terminal or reconnect attempts were not recorded");
        require(session.connect(credentials()), "Manual recovery did not restart the source");
        wait_until(
            [&]
            {
                const auto recovered = session.snapshot();
                return recovered.phase == live::LivePhase::Connected && recovered.room_id == "43" &&
                       recovered.accepted_events == 1;
            },
            "The recovered session did not admit a new event");
    }

    void check_source_exception_is_not_retried()
    {
        std::atomic<unsigned> calls{};
        const live::LiveSource source = [&](const auto&, std::stop_token, const auto& connected,
                                            const auto&, const auto&) -> live::LiveSourceFailure
        {
            ++calls;
            connected("42");
            throw std::logic_error("fixture source logic error");
        };
        live::LiveSession session(
            source,
            {.clock = fixed_time,
             .pause =
                 []
             {
             }},
            1, {.initial = std::chrono::milliseconds(10), .max = std::chrono::milliseconds(20)});

        require(session.connect(credentials()), "Internal failure fixture did not start");
        wait_until(
            [&]
            {
                return session.snapshot().phase == live::LivePhase::Failed;
            },
            "The source exception did not put the session in a failed state");
        const auto failed = session.snapshot();
        require(failed.last_error == "live_source_internal_error" && failed.room_id.empty() &&
                    !session.connected() && failed.reconnect_attempts == 0 && calls == 1,
                "A source logic error was treated as a network disconnect");
    }
}

int main()
{
    try
    {
        const std::vector<Admission> expected{
            {"chat", utc_ms - 59'000, monotonic_ms + 1'000},
            {"gift", utc_ms - 58'000, monotonic_ms + 2'000},
            {"enter", utc_ms - 57'000, monotonic_ms + 3'000},
            {"sc", utc_ms - 61'000, monotonic_ms + 39'000},
        };
        require(check_ttl(false) == expected && check_ttl(true) == expected,
                "Storage settings changed live admission or platform timestamps");
        check_storage_expiry();
        check_storage_results();
        check_disconnect_during_storage();
        check_retry_and_authorization_failure();
        check_source_exception_is_not_retried();
        std::println(
            "Live event expiry, persistence backpressure, session cleanup and retry passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Live session tests failed: {}", error.what());
        return 1;
    }
}
