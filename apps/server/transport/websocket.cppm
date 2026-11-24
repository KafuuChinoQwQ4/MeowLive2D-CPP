module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoop.h>

#if defined(_WIN32)
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#endif

export module meowlive.server.transport.websocket;

#if !defined(_WIN32)
import std;
#endif
import meowlive.protocol.control;
import meowlive.server.transport.access;

export namespace meowlive::server::transport
{
    struct BridgeSnapshot
    {
        std::string session_id;
        std::uint32_t generation{};
        bool connected{};
    };

    meowlive::server::transport::BridgeSnapshot bridge_snapshot();
    void register_websocket(std::shared_ptr<meowlive::server::transport::Access> access);
    void shutdown_websocket();
}

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr std::chrono::seconds handshake_timeout{5};
    constexpr std::chrono::seconds heartbeat_interval{10};
    constexpr std::chrono::seconds heartbeat_timeout{30};
    constexpr std::size_t control_limit{64 * 1024};
    constexpr std::size_t audio_limit{1024};
    const std::string reservation_key{"meowlive.websocket.reservation"};

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

    struct Endpoint
    {
        drogon::WebSocketConnectionPtr socket;
        trantor::EventLoop* loop{};

        void close(drogon::CloseCode code) const
        {
            if (!socket)
            {
                return;
            }
            loop->runInLoop(
                [socket = this->socket, loop = this->loop, code]
                {
                    if (socket->disconnected())
                    {
                        return;
                    }
                    socket->shutdown(code);
                    loop->runAfter(std::chrono::seconds{1},
                                   [weak_socket = std::weak_ptr(socket)]
                                   {
                                       if (const auto active = weak_socket.lock();
                                           active && !active->disconnected())
                                       {
                                           active->forceClose();
                                       }
                                   });
                });
        }
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
        meowlive::server::transport::BridgeSnapshot snapshot()
        {
            std::scoped_lock guard(mutex_);
            return {session_id_, generation_, active_ && active_->phase == Phase::Connected};
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
                ++generation_;
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
                request->getParameter("session_id") != session_id_ ||
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
            return meowlive::protocol::ServerHello{meowlive::protocol::version, session_id_,
                                                   active_->id, generation_};
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
            if (active_->phase == Phase::Connected)
            {
                ++generation_;
            }
            auto disconnected = std::move(*active_);
            active_.reset();
            return disconnected;
        }

        std::mutex mutex_;
        const std::string session_id_{drogon::utils::getUuid()};
        std::optional<Bridge> active_;
        std::uint32_t generation_{};
        bool stopping_{};
    };

    BridgeSession& bridge()
    {
        static BridgeSession state;
        return state;
    }

    void schedule_handshake_timeout()
    {
        drogon::app().getLoop()->runAfter(handshake_timeout,
                                          []
                                          {
                                              bridge().check_timeout();
                                          });
    }

    bool is_websocket_path(std::string_view path)
    {
        return path == meowlive::protocol::control_path || path == meowlive::protocol::audio_path;
    }

    drogon::HttpResponsePtr invalid_upgrade(const drogon::HttpRequestPtr& request)
    {
        auto connection = request->getHeader("connection");
        auto upgrade = request->getHeader("upgrade");
        const auto lower = [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        };
        std::ranges::transform(connection, connection.begin(), lower);
        std::ranges::transform(upgrade, upgrade.begin(), lower);
        if (!connection.contains("upgrade"))
        {
            return text_response(drogon::k400BadRequest,
                                 "Connection header did not include 'upgrade'");
        }
        if (upgrade != "websocket")
        {
            return text_response(drogon::k400BadRequest,
                                 "`Upgrade` header did not include 'websocket'");
        }
        if (request->getHeader("sec-websocket-key").empty())
        {
            return text_response(drogon::k400BadRequest, "`Sec-WebSocket-Key` header missing");
        }
        if (request->getHeader("sec-websocket-version") != "13")
        {
            return text_response(drogon::k400BadRequest,
                                 "`Sec-WebSocket-Version` header did not include '13'");
        }
        if (request->version() == drogon::Version::kHttp10)
        {
            return text_response(
                drogon::k426UpgradeRequired,
                "WebSocket request couldn't be upgraded since no upgrade state was present");
        }
        return {};
    }

    class BridgeAdmission final : public drogon::HttpFilter<BridgeAdmission, false>
    {
    public:
        void doFilter(const drogon::HttpRequestPtr& request, drogon::FilterCallback&& reject,
                      drogon::FilterChainCallback&& accept) override
        {
            if (!is_websocket_path(request->path()))
            {
                reject(drogon::HttpResponse::newNotFoundResponse(request));
                return;
            }
            const auto channel = request->path() == meowlive::protocol::control_path
                                     ? Channel::Control
                                     : Channel::Audio;
            auto reservation = bridge().reserve(channel, request);
            if (!reservation)
            {
                reject(reservation.error());
                return;
            }

            request->attributes()->insert(reservation_key, std::move(*reservation));
            if (channel == Channel::Control)
            {
                schedule_handshake_timeout();
            }
            accept();
        }
    };

    void start_heartbeat(const drogon::WebSocketConnectionPtr& socket)
    {
        socket->send("", drogon::WebSocketMessageType::Ping);
        socket->setPingMessage("", heartbeat_interval);
    }

    class BridgeController final : public drogon::WebSocketController<BridgeController, false>
    {
    public:
        static void initPathRouting()
        {
            BridgeController::registerSelf__(std::string{meowlive::protocol::control_path},
                                             {BridgeAdmission::classTypeName()});
            BridgeController::registerSelf__(std::string{meowlive::protocol::audio_path},
                                             {BridgeAdmission::classTypeName()});
        }

        void handleNewConnection(const drogon::HttpRequestPtr& request,
                                 const drogon::WebSocketConnectionPtr& socket) override
        {
            const auto& reservation = request->attributes()->get<Reservation>(reservation_key);
            socket->setContext(std::make_shared<Reservation>(reservation));
            socket->disablePing();
            const Endpoint endpoint{socket, trantor::EventLoop::getEventLoopOfCurrentThread()};
            if (reservation.bridge_id.empty() || !bridge().attach(reservation, endpoint))
            {
                endpoint.close(drogon::CloseCode::kEndpointGone);
                return;
            }
            if (reservation.channel == Channel::Audio)
            {
                start_heartbeat(socket);
            }
        }

        void handleNewMessage(const drogon::WebSocketConnectionPtr& socket, std::string&& message,
                              const drogon::WebSocketMessageType& type) override
        {
            const auto reservation = socket->getContext<Reservation>();
            if (!reservation)
            {
                return;
            }
            if (type == drogon::WebSocketMessageType::Close)
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kNormalClosure);
                return;
            }
            const auto phase = bridge().record_activity(*reservation);
            if (!phase)
            {
                return;
            }
            if (*phase != Phase::AwaitingHello && (type == drogon::WebSocketMessageType::Ping ||
                                                   type == drogon::WebSocketMessageType::Pong))
            {
                return;
            }
            const auto limit =
                reservation->channel == Channel::Control ? control_limit : audio_limit;
            if (message.size() > limit)
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kMessageTooBig);
                return;
            }
            if (reservation->channel != Channel::Control ||
                type != drogon::WebSocketMessageType::Text)
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kInvalidMessage);
                return;
            }
            auto decoded = meowlive::protocol::decode_client_message(message);
            if (!decoded)
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kProtocolError);
                return;
            }
            const auto* hello = std::get_if<meowlive::protocol::ClientHello>(&*decoded);
            if (*phase == Phase::AwaitingHello)
            {
                if (hello && hello->protocol_version == meowlive::protocol::version)
                {
                    if (const auto response = bridge().hello(*reservation))
                    {
                        schedule_handshake_timeout();
                        socket->send(meowlive::protocol::encode_server_command(*response));
                        start_heartbeat(socket);
                        return;
                    }
                }
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kProtocolError);
            }
            else if (hello)
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kProtocolError);
            }
            // 播报和资源用例接入后在此转交已通过编解码校验的客户端结果。
        }

        void handleConnectionClosed(const drogon::WebSocketConnectionPtr& socket) override
        {
            if (const auto reservation = socket->getContext<Reservation>())
            {
                bridge().disconnect(reservation->bridge_id, drogon::CloseCode::kEndpointGone);
            }
        }
    };
}

meowlive::server::transport::BridgeSnapshot meowlive::server::transport::bridge_snapshot()
{
    return bridge().snapshot();
}

void meowlive::server::transport::register_websocket(
    std::shared_ptr<meowlive::server::transport::Access> access)
{
    auto& app = drogon::app();
    app.setClientMaxWebSocketMessageSize(control_limit);
    app.registerPreRoutingAdvice(
        [access = std::move(access)](const drogon::HttpRequestPtr& request,
                                     drogon::AdviceCallback&& reject,
                                     drogon::AdviceChainCallback&& next)
        {
            if (request->method() == drogon::Get && is_websocket_path(request->path()))
            {
                if (const auto failure =
                        meowlive::server::transport::device_rejection(request, *access))
                {
                    reject(failure);
                    return;
                }
                if (const auto failure = invalid_upgrade(request))
                {
                    reject(failure);
                    return;
                }
            }
            next();
        });
    const auto ordinary_request = [](const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& callback)
    {
        callback(text_response(
            drogon::k426UpgradeRequired,
            "WebSocket request couldn't be upgraded since no upgrade state was present"));
    };
    app.registerHandler(std::string{meowlive::protocol::control_path}, auto(ordinary_request),
                        {drogon::Get});
    app.registerHandler(std::string{meowlive::protocol::audio_path}, auto(ordinary_request),
                        {drogon::Get});
    app.registerFilter(std::make_shared<BridgeAdmission>());
    app.registerController(std::make_shared<BridgeController>());
    app.getLoop()->runEvery(heartbeat_interval,
                            []
                            {
                                bridge().check_timeout();
                            });
}

void meowlive::server::transport::shutdown_websocket()
{
    bridge().shutdown();
}
