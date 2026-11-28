module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.memory_worker;

import meowlive.standard;

import meowlive.adapters.llm.language_model;
import meowlive.adapters.llm.memory_embedding;
import meowlive.adapters.storage.memory_jobs;
import meowlive.adapters.storage.memory_vectors;
import meowlive.adapters.storage.postgres;
import meowlive.application.memory_extraction;
import meowlive.platform.exception_diagnostics;

namespace
{
    std::int64_t memory_worker_time()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
}

export namespace meowlive::server
{
    class MemoryWorker
    {
    public:
        MemoryWorker(std::shared_ptr<meowlive::adapters::storage::MemoryJobs> jobs,
                     std::shared_ptr<meowlive::adapters::ProviderModel> model,
                     std::shared_ptr<meowlive::adapters::storage::MemoryVectors> vectors,
                     meowlive::adapters::EmbeddingConfig embedding, std::chrono::seconds timeout)
            : jobs_(std::move(jobs)), model_(std::move(model)), vectors_(std::move(vectors)),
              embedding_(std::move(embedding)), timeout_(timeout)
        {
            if (jobs_ || vectors_)
            {
                worker_ = std::jthread(
                    [this](std::stop_token stop)
                    {
                        run(stop);
                    });
            }
        }

        void close()
        {
            if (worker_.joinable())
            {
                worker_.request_stop();
                worker_.join();
            }
        }

    private:
        void extract(std::stop_token stop)
        {
            if (!jobs_ || !model_)
            {
                return;
            }
            if (const auto job = jobs_->claim(memory_worker_time()))
            {
                const auto candidates = meowlive::application::extract_memories(
                    *model_, std::span(&job->source, 1), memory_worker_time(), stop);
                if (candidates && !stop.stop_requested())
                {
                    jobs_->finish(*job, *candidates, memory_worker_time());
                }
                else
                {
                    jobs_->fail(*job, memory_worker_time());
                }
            }
        }

        void index(std::stop_token stop)
        {
            if (vectors_ && !stop.stop_requested())
            {
                if (const auto job = vectors_->claim(memory_worker_time()))
                {
                    const auto vector =
                        meowlive::adapters::embed_memory(embedding_, job->value, timeout_, stop);
                    if (vector && !stop.stop_requested())
                    {
                        vectors_->store(*job, *vector, memory_worker_time());
                    }
                }
            }
        }

        void run(std::stop_token stop)
        try
        {
            std::mutex mutex;
            std::condition_variable_any wait;
            std::unique_lock lock(mutex);
            auto cleanup_due = std::chrono::steady_clock::now();
            auto delay = std::chrono::seconds(2);
            while (!stop.stop_requested())
            {
                try
                {
                    if (jobs_ && std::chrono::steady_clock::now() >= cleanup_due)
                    {
                        jobs_->expire(memory_worker_time(), static_cast<bool>(model_));
                        cleanup_due = std::chrono::steady_clock::now() + std::chrono::minutes(1);
                    }
                    extract(stop);
                    index(stop);
                    delay = std::chrono::seconds(2);
                }
                catch (const meowlive::adapters::storage::PostgresUnavailable&)
                {
                    meowlive::platform::log_current_exception("memory.worker");
                    delay = std::chrono::seconds(15);
                }
                wait.wait_for(lock, stop, delay,
                              []
                              {
                                  return false;
                              });
            }
        }
        catch (const meowlive::adapters::storage::PostgresError&)
        {
            meowlive::platform::log_current_exception("memory.worker");
        }
        catch (...)
        {
            const auto kind = meowlive::platform::log_current_exception(
                "memory.worker", meowlive::platform::ExceptionDetails::Omit);
            if (kind == meowlive::platform::ExceptionKind::OutOfMemory)
            {
                std::terminate();
            }
        }

        const std::shared_ptr<meowlive::adapters::storage::MemoryJobs> jobs_;
        const std::shared_ptr<meowlive::adapters::ProviderModel> model_;
        const std::shared_ptr<meowlive::adapters::storage::MemoryVectors> vectors_;
        const meowlive::adapters::EmbeddingConfig embedding_;
        const std::chrono::seconds timeout_;
        std::jthread worker_;
    };
}
