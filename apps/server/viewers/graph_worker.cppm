module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.graph_worker;

import meowlive.standard;

import meowlive.adapters.storage.neo4j_projection;
import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.relationship_outbox;
import meowlive.platform.exception_diagnostics;

export namespace meowlive::server
{
    class GraphWorker
    {
    public:
        GraphWorker(std::string url, std::filesystem::path libpq, std::filesystem::path migrations,
                    std::string scope, const std::string& endpoint, std::string user,
                    std::string password)
            : database_(std::move(url), std::move(libpq), std::move(migrations), scope),
              graph_(endpoint, std::move(user), std::move(password)), scope_(std::move(scope)),
              worker_(
                  [this](std::stop_token stop)
                  {
                      run(stop);
                  })
        {
        }

        void close()
        {
            worker_.request_stop();
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

        ::Json::Value status()
        {
            auto result = database_.status(now_ms());
            result["enabled"] = true;
            result["connected"] = connected_.load();
            result["worker_failed"] = failed_.load();
            return result;
        }

        bool connected() const noexcept
        {
            return connected_.load();
        }

        std::optional<::Json::Value> references(const std::string& viewer) const
        {
            if (failed_)
            {
                throw meowlive::adapters::storage::Neo4jError("graph_worker_failed");
            }
            if (connected())
            {
                try
                {
                    return graph_.references(scope_, viewer);
                }
                catch (const meowlive::adapters::storage::Neo4jUnavailable&)
                {
                    meowlive::platform::log_current_exception("graph.references");
                }
            }
            return {};
        }

        std::optional<std::uint64_t> rebuild(const std::string& request, const std::string& reason)
        {
            if (failed_)
            {
                throw meowlive::adapters::storage::Neo4jError("graph_worker_failed");
            }
            const auto result = database_.rebuild(request, reason, now_ms());
            const std::lock_guard lock(mutex_);
            ready_ = true;
            wake_.notify_one();
            return result;
        }

    private:
        static std::uint64_t now_ms()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        void run(std::stop_token stop)
        try
        {
            bool prepared{};
            std::chrono::steady_clock::time_point health_check{};
            while (!stop.stop_requested())
            {
                try
                {
                    if (!prepared)
                    {
                        graph_.prepare(stop);
                        prepared = true;
                        connected_ = true;
                        health_check = std::chrono::steady_clock::now();
                    }
                    else if (!connected() || std::chrono::steady_clock::now() - health_check >
                                                 std::chrono::seconds(5))
                    {
                        graph_.query("RETURN 1 AS ready", ::Json::Value(::Json::objectValue), stop);
                        connected_ = true;
                        health_check = std::chrono::steady_clock::now();
                    }
                    if (auto lease = database_.claim(now_ms()))
                    {
                        bool success{};
                        try
                        {
                            graph_.project(lease->fact, stop);
                            success = true;
                            health_check = std::chrono::steady_clock::now();
                        }
                        catch (const meowlive::adapters::storage::Neo4jUnavailable&)
                        {
                            meowlive::platform::log_current_exception("graph.project");
                            connected_ = false;
                        }
                        if (!stop.stop_requested())
                        {
                            database_.finish(*lease, now_ms(), success);
                        }
                        if (success)
                        {
                            continue;
                        }
                    }
                }
                catch (const meowlive::adapters::storage::Neo4jCancelled&)
                {
                    break;
                }
                catch (const meowlive::adapters::storage::Neo4jUnavailable&)
                {
                    meowlive::platform::log_current_exception("graph.check_connection");
                    connected_ = false;
                }
                catch (const meowlive::adapters::storage::PostgresUnavailable&)
                {
                    meowlive::platform::log_current_exception("graph.outbox");
                    connected_ = false;
                }
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, stop, std::chrono::seconds(1),
                               [this]
                               {
                                   return ready_;
                               });
                ready_ = false;
            }
        }
        catch (const meowlive::adapters::storage::PostgresError&)
        {
            fail_worker(meowlive::platform::ExceptionDetails::Include);
        }
        catch (const meowlive::adapters::storage::Neo4jError&)
        {
            fail_worker(meowlive::platform::ExceptionDetails::Include);
        }
        catch (...)
        {
            fail_worker(meowlive::platform::ExceptionDetails::Omit);
        }

        void fail_worker(meowlive::platform::ExceptionDetails details) noexcept
        {
            const auto kind = meowlive::platform::log_current_exception("graph.worker", details);
            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
            failed_ = true;
            connected_ = false;
        }

        meowlive::adapters::storage::RelationshipOutbox database_;
        const meowlive::adapters::storage::Neo4jProjection graph_;
        const std::string scope_;
        std::mutex mutex_;
        std::condition_variable_any wake_;
        bool ready_{};
        std::atomic<bool> connected_{};
        std::atomic<bool> failed_{};
        std::jthread worker_;
    };
}
