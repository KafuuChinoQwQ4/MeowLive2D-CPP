module;

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <trantor/utils/Logger.h>
#include <cstdio>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.bootstrap;

import meowlive.standard;

import meowlive.adapters.bilibili.live_source;
import meowlive.adapters.llm.language_model;
import meowlive.adapters.llm.memory_embedding;
import meowlive.adapters.storage.repositories;
import meowlive.adapters.storage.memory_admin;
import meowlive.adapters.storage.memory_jobs;
import meowlive.adapters.storage.memory_vectors;
import meowlive.adapters.storage.reply_ledger;
import meowlive.adapters.storage.reply_outbox;
import meowlive.adapters.storage.viewer_queries;
import meowlive.adapters.storage.viewer_affinity;
import meowlive.adapters.storage.viewer_merge;
import meowlive.application.live_session;
import meowlive.application.memory_lookup;
import meowlive.application.reply_completion;
import meowlive.application.speech_worker;
import meowlive.domain.live_event;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.control;
import meowlive.server.agent_worker;
import meowlive.server.configuration;
import meowlive.server.graph_worker;
import meowlive.server.live;
import meowlive.server.live_settings;
import meowlive.server.managed_lifetime;
import meowlive.server.memory_worker;
import meowlive.server.profile_settings;
import meowlive.server.receipt_journal;
import meowlive.server.settings;
import meowlive.server.speech_voices;
#if defined(_WIN32)
import meowlive.server.training_jobs;
#endif
import meowlive.server.training_worker;
import meowlive.server.viewer_admin;
import meowlive.server.transport.access;
import meowlive.server.transport.http;
import meowlive.server.transport.training_limits;
import meowlive.server.transport.voice_http;
import meowlive.server.transport.websocket;

namespace
{
#if defined(_WIN32)
    using Argument = wchar_t;

    template <typename Service> class UpdateAdmission
    {
    public:
        explicit UpdateAdmission(Service& service)
            : service_(service), acquired_(service.freeze_for_update())
        {
        }

        UpdateAdmission(const UpdateAdmission&) = delete;
        UpdateAdmission& operator=(const UpdateAdmission&) = delete;

        ~UpdateAdmission()
        {
            if (acquired_)
            {
                service_.resume_after_update_check();
            }
        }

        bool acquired() const noexcept
        {
            return acquired_;
        }

        void keep_frozen() noexcept
        {
            acquired_ = false;
        }

    private:
        Service& service_;
        bool acquired_;
    };

    bool freeze_for_update(meowlive::application::SpeechWorker& speech,
                           meowlive::server::TrainingJobs& training,
                           meowlive::server::LiveService& live,
                           std::chrono::steady_clock::time_point deadline)
    {
        UpdateAdmission speech_gate(speech);
        if (!speech_gate.acquired())
        {
            return false;
        }
        UpdateAdmission training_gate(training);
        if (!training_gate.acquired())
        {
            return false;
        }
        UpdateAdmission live_gate(live);
        if (!live_gate.acquired() || std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        live_gate.keep_frozen();
        training_gate.keep_frozen();
        speech_gate.keep_frozen();
        return true;
    }
#else
    using Argument = char;
#endif
    struct Paths
    {
        std::filesystem::path config;
        std::filesystem::path data;
#if defined(_WIN32)
        bool managed{};
#endif
    };

    Paths parse_arguments(int count, Argument* const* arguments)
    {
        Paths paths;
        for (int index = 1; index < count; ++index)
        {
            const std::filesystem::path option(arguments[index]);
#if defined(_WIN32)
            if (option == "--managed" && !paths.managed)
            {
                paths.managed = true;
                continue;
            }
#endif
            auto* destination = option == "--config"     ? &paths.config
                                : option == "--data-dir" ? &paths.data
                                                         : nullptr;
            if (!destination || !destination->empty() || ++index == count || !*arguments[index])
            {
                throw std::runtime_error("用法：meowlive-server [--config PATH] [--data-dir PATH]");
            }
            *destination = arguments[index];
        }
        return paths;
    }

    struct ShutdownRequest
    {
        void request() noexcept
        {
            if (!started.exchange(true))
            {
                requested.release();
            }
        }

        std::binary_semaphore requested{0};
        std::atomic<bool> started{};
    };

    void finish_http_before_exit(std::stop_token stop, ShutdownRequest& shutdown,
                                 meowlive::server::transport::HttpWorkQueues& queues,
                                 meowlive::server::ProfileSettings& profiles,
                                 meowlive::server::TrainingWorker& training)
    {
        const std::stop_callback on_exit(stop,
                                         [&]
                                         {
                                             shutdown.request();
                                         });
        shutdown.requested.acquire();
        if (stop.stop_requested())
        {
            return;
        }
        LOG_INFO << "Stopping MeowLive2D server";
        queues.close();
        profiles.close();
        meowlive::server::transport::shutdown_websocket();
        queues.drain();
        profiles.drain();
        training.close();
        drogon::app().getLoop()->queueInLoop(
            []
            {
                drogon::app().quit();
            });
    }

    meowlive::application::LiveTime live_time()
    {
        using Milliseconds = std::chrono::milliseconds;
        return {
            static_cast<std::uint64_t>(std::chrono::duration_cast<Milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()),
            static_cast<std::uint64_t>(std::chrono::duration_cast<Milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count()),
        };
    }

    struct ViewerServices
    {
        std::shared_ptr<meowlive::adapters::storage::ViewerEvents> writer;
        std::shared_ptr<meowlive::server::ViewerAdmin> admin;
        std::shared_ptr<meowlive::adapters::storage::ReplyOutbox> replies;
        std::shared_ptr<meowlive::adapters::storage::MemoryAdmin> memory;
        std::shared_ptr<meowlive::adapters::storage::MemoryJobs> memory_jobs;
        std::shared_ptr<meowlive::adapters::storage::MemoryVectors> memory_vectors;
        meowlive::adapters::EmbeddingConfig embedding;
    };

    ViewerServices viewer_services(const meowlive::server::ViewerConfig& settings,
                                   const meowlive::server::MemoryConfig& memory,
                                   const std::filesystem::path& executable,
                                   const std::filesystem::path& receipt_directory)
    {
        if (!settings.enabled)
        {
            return {nullptr,
                    std::make_shared<meowlive::server::ViewerAdmin>(settings.scope_id, nullptr)};
        }
        const auto* url = std::getenv(settings.database_url_env.c_str());
        if (!url || !*url)
        {
            throw std::runtime_error("配置无效 [viewers.database_url_env]");
        }
        const auto migrations = settings.migrations.empty()
                                    ? executable.parent_path() / "migrations"
                                    : settings.migrations;
#if defined(_WIN32)
        if (settings.libpq.empty())
        {
            throw std::runtime_error("配置无效 [viewers.libpq_path]");
        }
#endif
        if (!std::filesystem::is_directory(migrations))
        {
            throw std::runtime_error("配置无效 [viewers.migrations_path]");
        }
        const auto ledger = std::make_shared<meowlive::adapters::storage::ReplyLedger>(
            url, settings.libpq, migrations, settings.scope_id,
            static_cast<int>(settings.calendar_offset_minutes));
        const auto memory_offset =
            memory.enabled ? std::optional(static_cast<int>(settings.calendar_offset_minutes))
                           : std::nullopt;
        meowlive::adapters::EmbeddingConfig embedding;
        if (!memory.embedding_model.empty())
        {
            const auto* key = memory.embedding_api_key_env.empty()
                                  ? nullptr
                                  : std::getenv(memory.embedding_api_key_env.c_str());
            embedding = {memory.embedding_endpoint, memory.embedding_model, key ? key : "",
                         memory.embedding_dimensions};
        }
        return {std::make_shared<meowlive::adapters::storage::ViewerEvents>(
                    url, settings.libpq, migrations, settings.scope_id, drogon::utils::getUuid(),
                    memory_offset),
                std::make_shared<meowlive::server::ViewerAdmin>(
                    settings.scope_id,
                    std::make_shared<meowlive::adapters::storage::ViewerQueries>(
                        url, settings.libpq, migrations, settings.scope_id),
                    std::make_shared<meowlive::adapters::storage::ViewerAffinity>(
                        url, settings.libpq, migrations, settings.scope_id,
                        static_cast<int>(settings.calendar_offset_minutes)),
                    std::make_shared<meowlive::adapters::storage::ViewerMerge>(
                        url, settings.libpq, migrations, settings.scope_id)),
                std::make_shared<meowlive::adapters::storage::ReplyOutbox>(
                    receipt_directory / "replies", receipt_directory / "reply-recovery",
                    settings.scope_id, drogon::utils::getUuid(),
                    [ledger](const meowlive::application::ReplyCompletion& completion)
                    {
                        ledger->complete(completion);
                    },
                    [](std::string_view code)
                    {
                        LOG_WARN << std::string(code);
                    }),
                std::make_shared<meowlive::adapters::storage::MemoryAdmin>(
                    url, settings.libpq, migrations, settings.scope_id, !embedding.model.empty()),
                memory.enabled || !embedding.model.empty()
                    ? std::make_shared<meowlive::adapters::storage::MemoryJobs>(
                          url, settings.libpq, migrations, settings.scope_id)
                    : nullptr,
                embedding.model.empty()
                    ? nullptr
                    : std::make_shared<meowlive::adapters::storage::MemoryVectors>(
                          url, settings.libpq, migrations, settings.scope_id, embedding.model,
                          embedding.dimensions),
                std::move(embedding)};
    }

    void create_runtime_directory(const std::filesystem::path& path, std::string_view role)
    {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
        {
            throw std::runtime_error("无法创建 " + std::string(role));
        }
    }
}

namespace meowlive::server
{
#if defined(_WIN32)
    int run(int count, wchar_t* const* arguments)
#else
    int run(int count, char* const* arguments)
#endif
    {
        meowlive::platform::install_terminate_handler();
        std::string_view phase = "server startup";
        try
        {
            const auto paths = parse_arguments(count, arguments);
            const auto config = meowlive::server::load_configuration(paths.data, paths.config);
            const auto logs = config.data_directory / "logs";
            const auto uploads = config.data_directory / "uploads";
            create_runtime_directory(logs, "data/logs");
            if (!meowlive::platform::open_exception_log(logs / "meowlive-server.exceptions.log"))
            {
                std::fputs("Cannot open meowlive-server.exceptions.log\n", stderr);
            }
            auto settings = meowlive::server::load_settings(config.config_path);
            const auto access = std::make_shared<meowlive::server::transport::Access>(config);
            create_runtime_directory(uploads, "data/uploads");

            const auto http_work = std::make_shared<meowlive::server::transport::HttpWorkQueues>(
                config.queue_capacity);
            auto& app = drogon::app();
            app.setLogPath(drogon::utils::fromNativePath(logs.native()), "meowlive-server",
                           5 * 1024 * 1024, 4)
                .setLogLevel(trantor::Logger::kInfo)
                .setUploadPath(drogon::utils::fromNativePath(uploads.native()))
                .setFileTypes({})
                .setImplicitPageEnable(false)
                .setClientMaxBodySize(meowlive::server::transport::training_upload_limit)
                .setClientMaxWebSocketMessageSize(64 * 1024)
                .addListener(config.address, config.port);
            app.registerBeginningAdvice(
                [loaded_count = settings.loaded_count(), queue_capacity = config.queue_capacity]
                {
                    LOG_INFO << "MeowLive2D server ready; local settings files: " << loaded_count
                             << "; work queue capacity: " << queue_capacity;
                });
            const auto journal =
                std::make_shared<meowlive::server::ReceiptJournal>(config.speech.receipt_directory);
            const auto viewers = viewer_services(config.viewers, config.memory,
                                                 std::filesystem::absolute(arguments[0]),
                                                 config.speech.receipt_directory);
            const auto executable_directory = std::filesystem::absolute(arguments[0]).parent_path();
            const auto training = std::make_shared<meowlive::server::TrainingWorker>(
                config.data_directory, executable_directory, config.speech.base_url);
            const auto voices = std::make_shared<meowlive::server::VoiceLibrary>(
                config.speech, training->model_control(),
                [training](std::string_view voice_id, std::stop_token stop)
                {
                    return training->jobs().weights_for_voice(voice_id, stop);
                });
            meowlive::application::ReserveReply reserve_reply;
            if (viewers.replies)
            {
                reserve_reply =
                    [outbox = viewers.replies](meowlive::application::ReplyCompletion completion)
                {
                    return outbox->reserve(std::move(completion));
                };
            }
            const auto speech = std::make_shared<meowlive::application::SpeechWorker>(
                config.queue_capacity, config.history_limit, voices->voices(),
                meowlive::application::SpeechWorkerCallbacks{
                    .new_id =
                        []
                    {
                        return drogon::utils::getUuid();
                    },
                    .record_receipt =
                        [journal](const std::string& session,
                                  const meowlive::application::PlaybackReceipt& receipt)
                    {
                        journal->append(session, receipt);
                    },
                    .report_synthesis_failure =
                        [](const std::string& id)
                    {
                        LOG_WARN << "speech_synthesis_failed " << id;
                    },
                    .reserve_reply = std::move(reserve_reply),
                });
            auto running_agent = std::make_shared<std::weak_ptr<meowlive::server::AgentWorker>>();
            const auto live = std::make_shared<meowlive::server::LiveService>(
                config.config_path,
                meowlive::server::initial_live_settings(config.live, settings.take_live()),
                meowlive::adapters::bilibili::run,
                meowlive::application::LiveSessionCallbacks{
                    .clock = live_time,
                    .persist =
                        [writer = viewers.writer](std::uint64_t session,
                                                  const meowlive::domain::LiveEvent& event)
                    {
                        return writer ? writer->append(session, event, live_time().utc_ms)
                                      : meowlive::application::LivePersistence::Stored;
                    },
                    .admit =
                        [running_agent, persisted = static_cast<bool>(viewers.writer)](
                            meowlive::domain::LiveEvent event, std::uint64_t deadline,
                            meowlive::application::LiveTime time)
                    {
                        if (const auto agent = running_agent->lock())
                        {
                            return agent->submit_live(std::move(event), deadline, time, persisted);
                        }
                        return meowlive::application::LiveAdmission::Rejected;
                    },
                    .pause =
                        [running_agent]
                    {
                        if (const auto agent = running_agent->lock())
                        {
                            agent->pause_live("bilibili");
                        }
                    },
                    .report =
                        [admin = viewers.admin](std::string_view code)
                    {
                        if (admin->enabled() && (code == "live_event_queue_full" ||
                                                 code == "live_event_storage_failed"))
                        {
                            admin->report_unconfirmed();
                        }
                        LOG_WARN << std::string(code);
                    },
                },
                config.queue_capacity, config.live.retry);
            auto defaults = meowlive::server::profile_defaults(config.config_path);
            const auto profiles = std::make_shared<meowlive::server::ProfileSettings>(
                config.config_path, std::move(settings), std::move(defaults.agent),
                std::move(defaults.llm));
            meowlive::application::ViewerMemoryQuery query_memory;
            if (config.viewers.agent_memory && viewers.admin->enabled())
            {
                query_memory = [admin = viewers.admin, embedding = viewers.embedding](
                                   const meowlive::domain::LiveEvent& event,
                                   std::chrono::seconds timeout, std::stop_token stop)
                {
                    return admin->memory(event, timeout, stop, embedding);
                };
            }
            const auto agent = std::make_shared<meowlive::server::AgentWorker>(
                speech, config.speech.resources_directory / "agent-runtime",
                meowlive::server::transport::query_obs,
                [weak = std::weak_ptr(live)]
                {
                    const auto service = weak.lock();
                    return service && service->connected();
                },
                std::move(query_memory));
            *running_agent = agent;
            std::shared_ptr<meowlive::server::GraphWorker> graph;
            if (config.graph.enabled)
            {
                const auto* url = std::getenv(config.viewers.database_url_env.c_str());
                const auto* password = std::getenv(config.graph.password_env.c_str());
                if (!password || !*password)
                {
                    throw std::runtime_error("配置无效 [graph.password_env]");
                }
                const auto migrations =
                    config.viewers.migrations.empty()
                        ? std::filesystem::absolute(arguments[0]).parent_path() / "migrations"
                        : config.viewers.migrations;
                graph = std::make_shared<meowlive::server::GraphWorker>(
                    url, config.viewers.libpq, migrations, config.viewers.scope_id,
                    config.graph.endpoint, config.graph.user, password);
            }
            agent->start(profiles->agent_settings(), profiles->model_config());
            std::shared_ptr<meowlive::adapters::ProviderModel> extraction_model;
            const auto memory_timeout = std::chrono::ceil<std::chrono::seconds>(
                std::chrono::milliseconds(config.memory.timeout_ms));
            if (config.memory.enabled)
            {
                const auto* key = config.memory.api_key_env.empty()
                                      ? nullptr
                                      : std::getenv(config.memory.api_key_env.c_str());
                meowlive::adapters::ProviderConfig provider;
                provider.base_url = config.memory.endpoint;
                provider.model = config.memory.model;
                provider.api_key = key ? key : "";
                provider.timeout = memory_timeout;
                provider.max_tokens = 2048;
                provider.json_mode = true;
                provider.stream = false;
                extraction_model =
                    std::make_shared<meowlive::adapters::ProviderModel>(std::move(provider));
            }
            meowlive::server::MemoryWorker memory_worker(
                viewers.memory_jobs, std::move(extraction_model), viewers.memory_vectors,
                viewers.embedding, memory_timeout);
            profiles->set_apply(
                [weak = std::weak_ptr(agent)](const ::Json::Value& updated, const ::Json::Value&)
                {
                    if (const auto running = weak.lock())
                    {
                        running->configure(updated);
                    }
                });
            const auto shutdown = std::make_shared<ShutdownRequest>();
            const auto on_signal = [shutdown]
            {
                shutdown->request();
            };
            app.setTermSignalHandler(on_signal).setIntSignalHandler(on_signal);
            meowlive::server::transport::register_http(access, speech, agent, profiles, http_work,
                                                       live, voices, training, viewers.admin,
                                                       viewers.memory, graph);
            meowlive::server::transport::register_websocket(access, speech);
            std::jthread shutdown_worker(
                [&](std::stop_token stop)
                {
                    finish_http_before_exit(stop, *shutdown, *http_work, *profiles, *training);
                });
            std::println("MeowLive2D protocol v{} ({}:{})", meowlive::protocol::version,
                         config.address, config.port);
            std::fflush(stdout);
#if defined(_WIN32)
            std::jthread managed;
            if (paths.managed)
            {
                managed = meowlive::server::watch_desktop_pipe(
                    on_signal,
                    [&](std::chrono::steady_clock::time_point deadline)
                    {
                        return freeze_for_update(*speech, training->jobs(), *live, deadline);
                    });
            }
#endif
            phase = "server event loop";
            app.run();
            phase = "server shutdown";
#if defined(_WIN32)
            managed.request_stop();
            if (managed.joinable())
            {
                managed.join();
            }
#endif
            shutdown_worker.request_stop();
            shutdown_worker.join();
            meowlive::server::transport::shutdown_websocket();
            http_work->drain();
            profiles->drain();
            if (graph)
            {
                graph->close();
            }
            training->close();
            memory_worker.close();
            live->close();
            agent->close();
            speech->close();
            const bool replies_saved = !viewers.replies || viewers.replies->close();
            const bool training_saved = !training->jobs().has_unsaved_outcome();
            if (!training_saved)
            {
                std::fputs("training_status_shutdown_pending\n", stderr);
            }
            return replies_saved && training_saved ? 0 : 1;
        }
        catch (const std::system_error&)
        {
            meowlive::platform::log_current_exception(phase,
                                                      meowlive::platform::ExceptionDetails::Omit);
        }
        catch (const std::runtime_error&)
        {
            const auto details = phase == "server startup"
                                     ? meowlive::platform::ExceptionDetails::Include
                                     : meowlive::platform::ExceptionDetails::Omit;
            meowlive::platform::log_current_exception(phase, details);
        }
        catch (...)
        {
            meowlive::platform::log_current_exception(phase,
                                                      meowlive::platform::ExceptionDetails::Omit);
        }
        return 1;
    }
}
