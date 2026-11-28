module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoop.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.bridge_session;

import meowlive.standard;
import meowlive.application.obs_status;
import meowlive.application.scheduler;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.protocol.control;
import meowlive.protocol.resources;
import meowlive.server.transport.access;

import meowlive.server.transport.audio_transfer;

export namespace meowlive::server::transport::bridge_detail
{
    using Clock = std::chrono::steady_clock;

    inline constexpr std::chrono::seconds handshake_timeout{5};
    inline constexpr std::chrono::seconds heartbeat_timeout{30};

    meowlive::application::PlaybackEvent playback_event(meowlive::protocol::ExecutionStatus status)
    {
        using Protocol = meowlive::protocol::ExecutionStatus;
        using Application = meowlive::application::PlaybackEvent;
        switch (status)
        {
        case Protocol::Started:
            return Application::Started;
        case Protocol::Completed:
            return Application::Completed;
        case Protocol::Cancelled:
            return Application::Cancelled;
        case Protocol::Failed:
            return Application::Failed;
        }
        std::unreachable();
    }

    enum class Channel
    {
        Control,
        Audio,
    };

    enum class Phase
    {
        AwaitingHello,
        AwaitingAudio,
        PairingAudio,
        Connected,
    };

    struct Reservation
    {
        std::string bridge_id;
        Channel channel{};
    };

    struct Bridge
    {
        std::string id;
        Clock::time_point deadline;
        Phase phase{Phase::AwaitingHello};
        Endpoint control;
        Endpoint audio;
        Clock::time_point last_control{};
        Clock::time_point last_audio{};
        std::uint32_t hello_generation{};
    };

    void close_bridge(const std::optional<Bridge>& bridge, drogon::CloseCode code)
    {
        if (bridge)
        {
            bridge->control.close(code);
            bridge->audio.close(code);
        }
    }

    drogon::HttpResponsePtr text_response(drogon::HttpStatusCode status, std::string body)
    {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(status);
        response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        response->setBody(std::move(body));
        return response;
    }

    class BridgeSession
    {
    public:
        void set_speech(std::shared_ptr<meowlive::application::SpeechWorker> speech)
        {
            speech_ = std::move(speech);
        }

        void receive(std::string_view bridge_id,
                     const meowlive::protocol::ExecutionReceipt& receipt)
        {
            speech_->receipt(bridge_id, {receipt.utterance_id, receipt.generation,
                                         playback_event(receipt.status), receipt.error});
        }

        std::optional<meowlive::protocol::DesktopResourceResult>
        query_resource(const meowlive::protocol::DesktopResourceOperation& operation,
                       std::chrono::seconds timeout, std::stop_token stop)
        {
            if (stop.stop_requested())
            {
                return {};
            }
            std::unique_lock lock(mutex_);
            if (!active_ || active_->phase != Phase::Connected || resource_)
            {
                return {};
            }
            const auto request_id = drogon::utils::getUuid();
            resource_.emplace(ResourceQuery{request_id, active_->id, {}});
            active_->control.send_control(
                meowlive::protocol::ResourceCommand{request_id, operation});
            resource_changed_.wait_for(lock, stop, timeout,
                                       [&]
                                       {
                                           return !resource_ ||
                                                  resource_->request_id != request_id ||
                                                  resource_->result.has_value();
                                       });
            if (!resource_ || resource_->request_id != request_id)
            {
                return {};
            }
            auto result = std::move(resource_->result);
            resource_.reset();
            return result;
        }

        void receive_resource(std::string_view bridge_id,
                              const meowlive::protocol::ResourceResultMessage& response)
        {
            std::lock_guard lock(mutex_);
            if (resource_ && resource_->bridge_id == bridge_id &&
                resource_->request_id == response.request_id && !resource_->result)
            {
                resource_->result = response.result;
                resource_changed_.notify_all();
            }
        }

        std::expected<Reservation, drogon::HttpResponsePtr>
        reserve(Channel channel, const drogon::HttpRequestPtr& request)
        {
            std::scoped_lock guard(mutex_);
            if (stopping_)
            {
                return std::unexpected(meowlive::server::transport::error_response(
                    drogon::k503ServiceUnavailable, "service_stopping", "服务正在停止"));
            }
            if (channel == Channel::Control)
            {
                if (active_)
                {
                    return std::unexpected(meowlive::server::transport::error_response(
                        drogon::k409Conflict, "bridge_busy", "已有执行端连接"));
                }
                active_.emplace(Bridge{drogon::utils::getUuid(), Clock::now() + handshake_timeout});
                speech_->reserve_bridge();
                return Reservation{active_->id, channel};
            }

            const auto& params = request->getParameters();
            for (const auto* name : {"session_id", "bridge_id"})
            {
                if (!params.contains(name))
                {
                    return std::unexpected(text_response(
                        drogon::k400BadRequest,
                        std::string{"Failed to deserialize query string: missing field `"} + name +
                            "`"));
                }
            }
            if (!active_ || active_->phase == Phase::AwaitingHello ||
                request->getParameter("session_id") != speech_->session_id() ||
                request->getParameter("bridge_id") != active_->id ||
                (active_->phase != Phase::Connected && Clock::now() >= active_->deadline))
            {
                return std::unexpected(meowlive::server::transport::error_response(
                    drogon::k409Conflict, "invalid_pair", "音频连接与当前执行端不匹配"));
            }
            if (active_->phase != Phase::AwaitingAudio)
            {
                return std::unexpected(meowlive::server::transport::error_response(
                    drogon::k409Conflict, "audio_busy", "音频连接已配对"));
            }
            active_->phase = Phase::PairingAudio;
            return Reservation{active_->id, channel};
        }

        bool attach(const Reservation& reservation, const Endpoint& endpoint)
        {
            std::scoped_lock guard(mutex_);
            if (!active_ || active_->id != reservation.bridge_id ||
                Clock::now() >= active_->deadline)
            {
                return false;
            }
            if (reservation.channel == Channel::Control)
            {
                active_->control = endpoint;
                active_->last_control = Clock::now();
            }
            else
            {
                active_->audio = endpoint;
                active_->last_audio = Clock::now();
                active_->phase = Phase::Connected;
                speech_->connect(
                    {
                        active_->id,
                        [](const meowlive::application::PcmAudio& pcm)
                        {
                            return meowlive::protocol::valid_audio_format(
                                {pcm.sample_rate, pcm.channels});
                        },
                        [control = active_->control, audio = active_->audio](
                            const std::string& id, std::uint32_t generation,
                            meowlive::application::PcmAudio pcm, std::stop_token stop)
                        {
                            const meowlive::protocol::ServerSpeak command{
                                id, generation, {pcm.sample_rate, pcm.channels}};
                            control.send_control(command);
                            std::make_shared<AudioTransfer>(audio, command, std::move(pcm),
                                                            std::move(stop))
                                ->start();
                        },
                        [control = active_->control](std::uint32_t generation)
                        {
                            control.send_control(meowlive::protocol::ServerStop{generation});
                        },
                    },
                    active_->hello_generation);
                LOG_INFO << "bridge_connected";
            }
            return true;
        }

        std::optional<Phase> record_activity(const Reservation& reservation)
        {
            std::scoped_lock guard(mutex_);
            if (!active_ || active_->id != reservation.bridge_id)
            {
                return std::nullopt;
            }

            auto& last = reservation.channel == Channel::Control ? active_->last_control
                                                                 : active_->last_audio;
            last = Clock::now();
            return active_->phase;
        }

        std::optional<meowlive::protocol::ServerHello> hello(const Reservation& reservation)
        {
            std::scoped_lock guard(mutex_);
            if (!active_ || active_->id != reservation.bridge_id ||
                active_->phase != Phase::AwaitingHello || Clock::now() >= active_->deadline)
            {
                return std::nullopt;
            }
            active_->phase = Phase::AwaitingAudio;
            active_->deadline = Clock::now() + handshake_timeout;
            active_->hello_generation = speech_->generation();
            return meowlive::protocol::ServerHello{meowlive::protocol::version,
                                                   speech_->session_id(), active_->id,
                                                   active_->hello_generation};
        }

        void disconnect(std::string_view bridge_id, drogon::CloseCode code)
        {
            std::optional<Bridge> disconnected;
            {
                std::scoped_lock guard(mutex_);
                if (active_ && active_->id == bridge_id)
                {
                    disconnected = detach();
                }
            }
            close_bridge(disconnected, code);
        }

        void check_timeout()
        {
            std::optional<Bridge> expired;
            {
                std::scoped_lock guard(mutex_);
                if (active_)
                {
                    const auto now = Clock::now();
                    const bool timeout = active_->phase == Phase::Connected
                                             ? now - active_->last_control > heartbeat_timeout ||
                                                   now - active_->last_audio > heartbeat_timeout
                                             : now >= active_->deadline;
                    if (timeout)
                    {
                        expired = detach();
                    }
                }
            }
            close_bridge(expired, drogon::CloseCode::kEndpointGone);
        }

        void shutdown()
        {
            std::optional<Bridge> disconnected;
            {
                std::scoped_lock guard(mutex_);
                stopping_ = true;
                if (active_)
                {
                    disconnected = detach();
                }
            }
            close_bridge(disconnected, drogon::CloseCode::kEndpointGone);
        }

    private:
        Bridge detach()
        {
            LOG_INFO << "bridge_disconnected";
            speech_->disconnect();
            resource_.reset();
            resource_changed_.notify_all();
            auto disconnected = std::move(*active_);
            active_.reset();
            return disconnected;
        }

        struct ResourceQuery
        {
            std::string request_id;
            std::string bridge_id;
            std::optional<meowlive::protocol::DesktopResourceResult> result;
        };

        std::condition_variable_any resource_changed_;
        std::optional<ResourceQuery> resource_;
        std::mutex mutex_;
        std::shared_ptr<meowlive::application::SpeechWorker> speech_;
        std::optional<Bridge> active_;
        bool stopping_{};
    };

    BridgeSession& bridge()
    {
        static BridgeSession state;
        return state;
    }
}
