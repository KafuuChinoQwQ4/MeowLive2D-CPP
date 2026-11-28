#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>
#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

import meowlive.desktop_runtime.connection;
import meowlive.protocol.control;

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    template <typename Predicate> void wait_for(Predicate ready, std::string_view message)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!ready())
        {
            require(std::chrono::steady_clock::now() < deadline, message);
            ::QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    class Bridge
    {
    public:
        Bridge()
            : server_(QStringLiteral("Stage 3 bridge fixture"), ::QWebSocketServer::NonSecureMode)
        {
            require(server_.listen(::QHostAddress::LocalHost), "Cannot start fixture bridge");
            ::QObject::connect(&server_, &::QWebSocketServer::newConnection, &server_,
                               [this]
                               {
                                   this->next_connection();
                               });
        }

        ::QUrl url() const
        {
            return server_.serverUrl();
        }

        void await_pair(unsigned count)
        {
            wait_for(
                [this, count]
                {
                    return connected_ >= count;
                },
                "Qt executor did not pair with the fixture");
            this->barrier(*audio_);
        }

        void speak(const std::string& id, std::uint32_t generation)
        {
            this->command(meowlive::protocol::ServerSpeak{id, generation, {24'000, 1}});
        }

        void stop(std::uint32_t generation)
        {
            this->command(meowlive::protocol::ServerStop{generation});
        }

        void frame(const std::string& id, std::uint32_t generation, std::uint32_t sequence = 0)
        {
            const auto frame = meowlive::protocol::encode_audio_chunk({.utterance_id = id,
                                                                       .generation = generation,
                                                                       .sequence = sequence,
                                                                       .samples = {},
                                                                       .end = true});
            require(frame.has_value(), "Cannot encode fixture PCM");
            audio_->sendBinaryMessage(::QByteArray(reinterpret_cast<const char*>(frame->data()),
                                                   static_cast<::qsizetype>(frame->size())));
        }

        void await_audio()
        {
            this->barrier(*audio_);
        }

        void await_control()
        {
            this->barrier(*control_);
        }

        void check_obs_resource()
        {
            ::Json::Value operation(::Json::objectValue);
            operation["type"] = "obs";
            operation["operation"]["type"] = "status";
            this->command(meowlive::protocol::ResourceCommand{
                "read-obs",
                meowlive::protocol::DesktopResourceOperation::decode(operation).value()});
            operation["operation"]["type"] = "start_recording";
            this->command(meowlive::protocol::ResourceCommand{
                "write-obs",
                meowlive::protocol::DesktopResourceOperation::decode(operation).value()});
            wait_for(
                [this]
                {
                    return resources_.size() == 2;
                },
                "OBS resource did not return");
            const auto& status = resources_[0].result.json();
            require(resources_[0].request_id == "read-obs" && status["type"] == "obs" &&
                        !status["snapshot"]["connected"].asBool() &&
                        !status["snapshot"]["recording"].asBool() &&
                        status["snapshot"]["current_scene"].asString().empty() &&
                        status["snapshot"]["scenes"].empty(),
                    "OBS snapshot did not report an unavailable desktop backend");
            require(resources_[1].request_id == "write-obs" &&
                        resources_[1].result.json()["type"] == "error" &&
                        resources_[1].result.json()["code"] == "resource_unsupported",
                    "OBS recording request was not rejected");
        }

        void expect_failed(std::size_t index, std::string_view id, std::uint32_t generation)
        {
            wait_for(
                [this, index]
                {
                    return receipts_.size() > index;
                },
                "Qt executor did not send a terminal receipt");
            const auto& receipt = receipts_[index];
            require(receipt.utterance_id == id && receipt.generation == generation &&
                        receipt.status == meowlive::protocol::ExecutionStatus::Failed &&
                        receipt.error == "utterance contains no PCM samples",
                    "Qt executor sent an unexpected playback receipt");
        }

        std::size_t receipts() const
        {
            return receipts_.size();
        }

        void restart()
        {
            const auto port = server_.serverPort();
            server_.close();
            audio_->abort();
            control_->abort();
            require(server_.listen(::QHostAddress::LocalHost, port),
                    "Fixture could not reopen the same port");
        }

    private:
        void next_connection()
        {
            auto* socket = server_.nextPendingConnection();
            socket->setParent(&server_);
            ::QObject::connect(socket, &::QWebSocket::pong, &server_,
                               [this]
                               {
                                   ++pongs_;
                               });
            if (socket->requestUrl().path() == QStringLiteral("/ws/control"))
            {
                control_ = socket;
                ::QObject::connect(socket, &::QWebSocket::textMessageReceived, &server_,
                                   [this, socket](const ::QString& message)
                                   {
                                       if (socket == control_)
                                       {
                                           this->receive(message);
                                       }
                                   });
            }
            else if (socket->requestUrl().path() == QStringLiteral("/ws/audio"))
            {
                audio_ = socket;
                ++connected_;
            }
        }

        void receive(const ::QString& message)
        {
            const auto bytes = message.toUtf8();
            auto decoded = meowlive::protocol::decode_client_message(
                {bytes.constData(), static_cast<std::size_t>(bytes.size())});
            if (!decoded)
            {
                return;
            }
            if (std::get_if<meowlive::protocol::ClientHello>(&*decoded))
            {
                this->command(meowlive::protocol::ServerHello{
                    meowlive::protocol::version, "fixture-" + std::to_string(connected_), "bridge",
                    7});
            }
            else if (const auto* receipt =
                         std::get_if<meowlive::protocol::ExecutionReceipt>(&*decoded))
            {
                receipts_.push_back(*receipt);
            }
            else if (const auto* resource =
                         std::get_if<meowlive::protocol::ResourceResultMessage>(&*decoded))
            {
                resources_.push_back(*resource);
            }
        }

        void command(const meowlive::protocol::ServerCommand& command)
        {
            const auto encoded = meowlive::protocol::encode_server_command(command);
            control_->sendTextMessage(
                ::QString::fromUtf8(encoded.data(), static_cast<::qsizetype>(encoded.size())));
        }

        void barrier(::QWebSocket& socket)
        {
            const auto before = pongs_;
            socket.ping();
            wait_for(
                [this, before]
                {
                    return pongs_ != before;
                },
                "WebSocket messages did not pass the fixture barrier");
        }

        ::QWebSocketServer server_;
        ::QWebSocket* control_{};
        ::QWebSocket* audio_{};
        unsigned connected_{};
        unsigned pongs_{};
        std::vector<meowlive::protocol::ExecutionReceipt> receipts_;
        std::vector<meowlive::protocol::ResourceResultMessage> resources_;
    };

    void check_playback_protocol()
    {
        Bridge bridge;
        meowlive::desktop_runtime::ServerConnection client(bridge.url(), {},
                                                           [](const ::QString&)
                                                           {
                                                           });
        client.start();
        bridge.await_pair(1);
        bridge.check_obs_resource();

        bridge.frame("early", 7);
        bridge.await_audio();
        require(bridge.receipts() == 0, "Early audio produced a receipt without a command");
        bridge.speak("early", 7);
        bridge.expect_failed(0, "early", 7);

        bridge.frame("future", 8);
        bridge.frame("discarded-on-stop", 7);
        bridge.await_audio();
        bridge.stop(8);
        bridge.speak("old", 7);
        bridge.await_control();
        for (int frame = 0; frame < 65; ++frame)
        {
            bridge.frame("old", 7);
        }
        bridge.await_audio();
        bridge.speak("future", 8);
        bridge.expect_failed(1, "future", 8);

        bridge.speak("sequence-gap", 8);
        bridge.await_control();
        bridge.frame("sequence-gap", 8, 1);
        bridge.await_pair(2);
        require(bridge.receipts() == 2, "Sequence gap produced a playback receipt");

        for (int frame = 0; frame < 64; ++frame)
        {
            bridge.frame("overflow", 7, static_cast<std::uint32_t>(frame));
        }
        bridge.await_audio();
        bridge.frame("overflow", 7, 64);
        bridge.await_pair(3);
        require(bridge.receipts() == 2, "Buffer overflow produced a playback receipt");

        bridge.frame("fresh-after-restart", 7, 1);
        bridge.await_audio();
        bridge.restart();
        bridge.await_pair(4);
        bridge.speak("fresh-after-restart", 7);
        bridge.await_control();
        bridge.frame("fresh-after-restart", 7);
        bridge.expect_failed(2, "fresh-after-restart", 7);
        require(bridge.receipts() == 3, "Restart left stale playback receipts");
    }
}

int main(int argc, char** argv)
{
    const ::QCoreApplication application(argc, argv);
    try
    {
        check_playback_protocol();
        std::puts("Stage 3 Qt playback protocol passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
