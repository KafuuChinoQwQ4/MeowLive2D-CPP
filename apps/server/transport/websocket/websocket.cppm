module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoop.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.websocket;

import meowlive.standard;
import meowlive.application.obs_status;
import meowlive.application.scheduler;
import meowlive.application.speech;
import meowlive.application.speech_worker;
import meowlive.protocol.control;
import meowlive.protocol.resources;
import meowlive.server.transport.access;

import meowlive.server.transport.audio_transfer;
import meowlive.server.transport.bridge_session;

export namespace meowlive::server::transport
{
    void register_websocket(std::shared_ptr<meowlive::server::transport::Access> access,
                            const std::shared_ptr<meowlive::application::SpeechWorker>& speech);
    void shutdown_websocket();
    std::optional<meowlive::protocol::DesktopResourceResult>
    query_resource(const meowlive::protocol::DesktopResourceOperation& operation,
                   std::chrono::seconds timeout, std::stop_token stop = {});
    std::optional<meowlive::application::ObsStatus> query_obs(std::chrono::seconds timeout,
                                                              std::stop_token stop);
}

namespace
{
    constexpr std::chrono::seconds heartbeat_interval{10};
    constexpr std::size_t control_limit{64 * 1024};
    constexpr std::size_t audio_limit{1024};
    const std::string reservation_key{"meowlive.websocket.reservation"};

    void schedule_handshake_timeout()
    {
        drogon::app().getLoop()->runAfter(
            meowlive::server::transport::bridge_detail::handshake_timeout,
            []
            {
                meowlive::server::transport::bridge_detail::bridge().check_timeout();
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
            return meowlive::server::transport::bridge_detail::text_response(
                drogon::k400BadRequest, "Connection header did not include 'upgrade'");
        }
        if (upgrade != "websocket")
        {
            return meowlive::server::transport::bridge_detail::text_response(
                drogon::k400BadRequest, "`Upgrade` header did not include 'websocket'");
        }
        if (request->getHeader("sec-websocket-key").empty())
        {
            return meowlive::server::transport::bridge_detail::text_response(
                drogon::k400BadRequest, "`Sec-WebSocket-Key` header missing");
        }
        if (request->getHeader("sec-websocket-version") != "13")
        {
            return meowlive::server::transport::bridge_detail::text_response(
                drogon::k400BadRequest, "`Sec-WebSocket-Version` header did not include '13'");
        }
        if (request->version() == drogon::Version::kHttp10)
        {
            return meowlive::server::transport::bridge_detail::text_response(
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
                                     ? meowlive::server::transport::bridge_detail::Channel::Control
                                     : meowlive::server::transport::bridge_detail::Channel::Audio;
            auto reservation =
                meowlive::server::transport::bridge_detail::bridge().reserve(channel, request);
            if (!reservation)
            {
                reject(reservation.error());
                return;
            }

            request->attributes()->insert(reservation_key, std::move(*reservation));
            if (channel == meowlive::server::transport::bridge_detail::Channel::Control)
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
            const auto& reservation =
                request->attributes()->get<meowlive::server::transport::bridge_detail::Reservation>(
                    reservation_key);
            socket->setContext(
                std::make_shared<meowlive::server::transport::bridge_detail::Reservation>(
                    reservation));
            socket->disablePing();
            const meowlive::server::transport::bridge_detail::Endpoint endpoint{
                socket, trantor::EventLoop::getEventLoopOfCurrentThread()};
            if (reservation.bridge_id.empty() ||
                !meowlive::server::transport::bridge_detail::bridge().attach(reservation, endpoint))
            {
                endpoint.close(drogon::CloseCode::kEndpointGone);
                return;
            }
            if (reservation.channel == meowlive::server::transport::bridge_detail::Channel::Audio)
            {
                start_heartbeat(socket);
            }
        }

        void handleNewMessage(const drogon::WebSocketConnectionPtr& socket, std::string&& message,
                              const drogon::WebSocketMessageType& type) override
        {
            const auto reservation =
                socket->getContext<meowlive::server::transport::bridge_detail::Reservation>();
            if (!reservation)
            {
                return;
            }
            if (type == drogon::WebSocketMessageType::Close)
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kNormalClosure);
                return;
            }
            const auto phase =
                meowlive::server::transport::bridge_detail::bridge().record_activity(*reservation);
            if (!phase)
            {
                return;
            }
            if (*phase != meowlive::server::transport::bridge_detail::Phase::AwaitingHello &&
                (type == drogon::WebSocketMessageType::Ping ||
                 type == drogon::WebSocketMessageType::Pong))
            {
                return;
            }
            const auto limit =
                reservation->channel == meowlive::server::transport::bridge_detail::Channel::Control
                    ? control_limit
                    : audio_limit;
            if (message.size() > limit)
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kMessageTooBig);
                return;
            }
            if (reservation->channel !=
                    meowlive::server::transport::bridge_detail::Channel::Control ||
                type != drogon::WebSocketMessageType::Text)
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kInvalidMessage);
                return;
            }
            auto decoded = meowlive::protocol::decode_client_message(message);
            if (!decoded)
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kProtocolError);
                return;
            }
            const auto* hello = std::get_if<meowlive::protocol::ClientHello>(&*decoded);
            if (*phase == meowlive::server::transport::bridge_detail::Phase::AwaitingHello)
            {
                if (hello && hello->protocol_version == meowlive::protocol::version)
                {
                    if (const auto response =
                            meowlive::server::transport::bridge_detail::bridge().hello(
                                *reservation))
                    {
                        schedule_handshake_timeout();
                        socket->send(meowlive::protocol::encode_server_command(*response));
                        start_heartbeat(socket);
                        return;
                    }
                }
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kProtocolError);
            }
            else if (hello)
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kProtocolError);
            }
            else if (*phase == meowlive::server::transport::bridge_detail::Phase::Connected)
            {
                if (const auto* receipt =
                        std::get_if<meowlive::protocol::ExecutionReceipt>(&*decoded))
                {
                    meowlive::server::transport::bridge_detail::bridge().receive(
                        reservation->bridge_id, *receipt);
                }
                else if (const auto* resource =
                             std::get_if<meowlive::protocol::ResourceResultMessage>(&*decoded))
                {
                    meowlive::server::transport::bridge_detail::bridge().receive_resource(
                        reservation->bridge_id, *resource);
                }
            }
        }

        void handleConnectionClosed(const drogon::WebSocketConnectionPtr& socket) override
        {
            if (const auto reservation =
                    socket->getContext<meowlive::server::transport::bridge_detail::Reservation>())
            {
                meowlive::server::transport::bridge_detail::bridge().disconnect(
                    reservation->bridge_id, drogon::CloseCode::kEndpointGone);
            }
        }
    };
}

void meowlive::server::transport::register_websocket(
    std::shared_ptr<meowlive::server::transport::Access> access,
    const std::shared_ptr<meowlive::application::SpeechWorker>& speech)
{
    meowlive::server::transport::bridge_detail::bridge().set_speech(speech);
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
        callback(meowlive::server::transport::bridge_detail::text_response(
            drogon::k426UpgradeRequired,
            "WebSocket request couldn't be upgraded since no upgrade state was present"));
    };
    app.registerHandler(std::string{meowlive::protocol::control_path}, auto(ordinary_request),
                        {drogon::Get});
    app.registerHandler(std::string{meowlive::protocol::audio_path}, auto(ordinary_request),
                        {drogon::Get});
    app.registerFilter(std::make_shared<BridgeAdmission>());
    app.registerController(std::make_shared<BridgeController>());
    app.getLoop()->runEvery(
        heartbeat_interval,
        []
        {
            meowlive::server::transport::bridge_detail::bridge().check_timeout();
        });
}

void meowlive::server::transport::shutdown_websocket()
{
    meowlive::server::transport::bridge_detail::bridge().shutdown();
}

std::optional<meowlive::protocol::DesktopResourceResult>
meowlive::server::transport::query_resource(
    const meowlive::protocol::DesktopResourceOperation& operation, std::chrono::seconds timeout,
    std::stop_token stop)
{
    return meowlive::server::transport::bridge_detail::bridge().query_resource(operation, timeout,
                                                                               stop);
}

std::optional<meowlive::application::ObsStatus>
meowlive::server::transport::query_obs(std::chrono::seconds timeout, std::stop_token stop)
{
    ::Json::Value input(::Json::objectValue);
    input["type"] = "obs";
    input["operation"]["type"] = "status";
    const auto operation = meowlive::protocol::DesktopResourceOperation::decode(input);
    const auto result = meowlive::server::transport::query_resource(*operation, timeout, stop);
    if (!result || result->json()["type"] != "obs")
    {
        return {};
    }
    const auto& snapshot = result->json()["snapshot"];
    return meowlive::application::ObsStatus{
        .connected = snapshot["connected"].asBool(),
        .recording = snapshot["recording"].asBool(),
        .current_scene = snapshot["current_scene"].asString(),
    };
}
