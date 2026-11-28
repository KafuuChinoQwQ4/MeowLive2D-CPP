module;

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkRequest>
#include <QtWebSockets/QWebSocket>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.desktop_runtime.connection;

import meowlive.standard;
import meowlive.protocol.audio;
import meowlive.protocol.control;
import meowlive.desktop_runtime.audio.playback;
import meowlive.desktop_runtime.avatar.presentation;
import meowlive.desktop_runtime.avatar.resources;
import meowlive.desktop_runtime.obs.client;

export namespace meowlive::desktop_runtime
{
    class ServerConnection final : public ::QObject
    {
    public:
        ServerConnection(::QUrl origin, ::QByteArray token,
                         std::function<void(const ::QString&)> status,
                         meowlive::desktop_runtime::obs::Client* obs = nullptr,
                         meowlive::desktop_runtime::avatar::Presentation* avatar = nullptr)
            : origin_(std::move(origin)), token_(std::move(token)), status_(std::move(status)),
              obs_(obs), avatar_(avatar)
        {
            control_.setProxy(::QNetworkProxy::NoProxy);
            control_.setMaxAllowedIncomingFrameSize(65'536);
            control_.setMaxAllowedIncomingMessageSize(65'536);
            audio_.setProxy(::QNetworkProxy::NoProxy);
            audio_.setMaxAllowedIncomingFrameSize(meowlive::protocol::max_audio_frame_bytes);
            audio_.setMaxAllowedIncomingMessageSize(meowlive::protocol::max_audio_frame_bytes);

            handshake_.setSingleShot(true);
            handshake_.setInterval(5'000);
            retry_.setSingleShot(true);
            retry_.setInterval(2'000);
            heartbeat_.setInterval(10'000);
            receipts_.setInterval(15);

            this->connect(&retry_, &::QTimer::timeout, this, &ServerConnection::connect_control);
            this->connect(&handshake_, &::QTimer::timeout, this,
                          [this]
                          {
                              this->retry(QStringLiteral("执行端配对超时，正在重连"));
                          });
            this->connect(&heartbeat_, &::QTimer::timeout, this, &ServerConnection::ping);
            this->connect(&receipts_, &::QTimer::timeout, this, &ServerConnection::send_receipts);
            this->connect(&control_, &::QWebSocket::connected, this,
                          &ServerConnection::client_hello);
            this->connect(&audio_, &::QWebSocket::connected, this, &ServerConnection::paired);
            this->connect(&control_, &::QWebSocket::textMessageReceived, this,
                          &ServerConnection::receive_control);
            this->connect(&audio_, &::QWebSocket::binaryMessageReceived, this,
                          &ServerConnection::receive_audio);
            this->connect(&control_, &::QWebSocket::binaryMessageReceived, this,
                          [this]
                          {
                              this->retry(QStringLiteral("控制通道需要 JSON 文本，正在重连"));
                          });
            this->connect(&audio_, &::QWebSocket::textMessageReceived, this,
                          [this]
                          {
                              this->retry(QStringLiteral("音频通道需要 PCM 帧，正在重连"));
                          });
            this->connect(&control_, &::QWebSocket::pong, this,
                          [this]
                          {
                              control_alive_.start();
                          });
            this->connect(&audio_, &::QWebSocket::pong, this,
                          [this]
                          {
                              audio_alive_.start();
                          });
            this->connect(&control_, &::QWebSocket::disconnected, this,
                          [this]
                          {
                              this->retry(QStringLiteral("控制通道已断开，正在重连"));
                          });
            this->connect(&audio_, &::QWebSocket::disconnected, this,
                          [this]
                          {
                              this->retry(QStringLiteral("音频通道已断开，正在重连"));
                          });
            this->connect(&control_, &::QWebSocket::errorOccurred, this,
                          [this]
                          {
                              this->retry(QStringLiteral("控制通道连接失败，正在重连"));
                          });
            this->connect(&audio_, &::QWebSocket::errorOccurred, this,
                          [this]
                          {
                              this->retry(QStringLiteral("音频通道连接失败，正在重连"));
                          });
        }

        void start()
        {
            if (stage_ == Stage::Idle)
            {
                this->connect_control();
            }
        }

        double mouth_level() const noexcept
        {
            return playback_.level();
        }

    private:
        enum class Stage
        {
            Idle,
            ControlConnecting,
            AwaitingHello,
            AudioConnecting,
            Paired,
            Reconnecting,
        };

        ::QUrl endpoint(std::string_view path) const
        {
            ::QUrl url = origin_;
            url.setScheme(QStringLiteral("ws"));
            url.setPath(::QString::fromUtf8(path.data(), static_cast<::qsizetype>(path.size())));
            return url;
        }

        ::QNetworkRequest request(const ::QUrl& url) const
        {
            ::QNetworkRequest result(url);
            if (!token_.isEmpty())
            {
                result.setRawHeader(QByteArrayLiteral("Authorization"),
                                    QByteArrayLiteral("Bearer ") + token_);
            }
            return result;
        }

        void connect_control()
        {
            stage_ = Stage::ControlConnecting;
            status_(QStringLiteral("执行端连接中"));
            handshake_.start();
            control_.open(this->request(this->endpoint(meowlive::protocol::control_path)));
        }

        void client_hello()
        {
            if (stage_ != Stage::ControlConnecting)
            {
                return;
            }
            stage_ = Stage::AwaitingHello;
            handshake_.start();
            const auto hello = meowlive::protocol::encode_client_message(
                meowlive::protocol::ClientHello{meowlive::protocol::version});
            control_.sendTextMessage(
                ::QString::fromUtf8(hello.data(), static_cast<::qsizetype>(hello.size())));
        }

        void receive_control(const ::QString& text)
        {
            if (stage_ == Stage::Reconnecting)
            {
                return;
            }
            const auto bytes = text.toUtf8();
            const auto command = meowlive::protocol::decode_server_command(
                std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
            if (!command)
            {
                this->retry(QStringLiteral("控制消息无效，正在重连"));
                return;
            }

            const auto* hello = std::get_if<meowlive::protocol::ServerHello>(&*command);
            if (stage_ == Stage::AwaitingHello)
            {
                if (hello == nullptr || hello->protocol_version != meowlive::protocol::version ||
                    hello->session_id.empty() || hello->bridge_id.empty())
                {
                    this->retry(QStringLiteral("服务端握手无效，正在重连"));
                    return;
                }

                ::QUrl url = this->endpoint(meowlive::protocol::audio_path);
                const auto session = ::QString::fromUtf8(
                    hello->session_id.data(), static_cast<::qsizetype>(hello->session_id.size()));
                const auto bridge = ::QString::fromUtf8(
                    hello->bridge_id.data(), static_cast<::qsizetype>(hello->bridge_id.size()));
                const auto query =
                    QByteArrayLiteral("session_id=") + ::QUrl::toPercentEncoding(session) +
                    QByteArrayLiteral("&bridge_id=") + ::QUrl::toPercentEncoding(bridge);
                url.setQuery(::QString::fromLatin1(query), ::QUrl::StrictMode);
                playback_.reset(hello->generation);
                stage_ = Stage::AudioConnecting;
                handshake_.start();
                audio_.open(this->request(url));
                return;
            }
            if (hello != nullptr)
            {
                this->retry(QStringLiteral("收到重复的服务端握手，正在重连"));
                return;
            }
            control_alive_.start();
            if (const auto* speak = std::get_if<meowlive::protocol::ServerSpeak>(&*command))
            {
                if (!playback_.speak(*speak))
                {
                    this->retry(QStringLiteral("播报指令无效，正在重连"));
                }
            }
            else if (const auto* stop = std::get_if<meowlive::protocol::ServerStop>(&*command))
            {
                playback_.stop(stop->generation);
            }
            else if (const auto* resource =
                         std::get_if<meowlive::protocol::ResourceCommand>(&*command);
                     resource != nullptr && stage_ == Stage::Paired)
            {
                this->reply_resource(*resource);
            }
        }

        void paired()
        {
            if (stage_ != Stage::AudioConnecting)
            {
                return;
            }
            stage_ = Stage::Paired;
            handshake_.stop();
            control_alive_.start();
            audio_alive_.start();
            heartbeat_.start();
            receipts_.start();
            status_(QStringLiteral("执行端已配对"));
        }

        void receive_audio(const ::QByteArray& message)
        {
            if (stage_ != Stage::Paired)
            {
                return;
            }
            const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(message.constData()),
                                         static_cast<std::size_t>(message.size()));
            auto chunk = meowlive::protocol::decode_audio_chunk(bytes);
            if (!chunk || !playback_.receive(std::move(*chunk)))
            {
                this->retry(QStringLiteral("音频帧无效，正在重连"));
                return;
            }
            audio_alive_.start();
        }

        void send_receipts()
        {
            for (const auto& receipt : playback_.poll())
            {
                const auto message = meowlive::protocol::encode_client_message(receipt);
                if (control_.sendTextMessage(::QString::fromUtf8(
                        message.data(), static_cast<::qsizetype>(message.size()))) <= 0)
                {
                    this->retry(QStringLiteral("发送播报回执失败，正在重连"));
                    return;
                }
            }
        }

        void reply_resource(const meowlive::protocol::ResourceCommand& resource)
        {
            const auto& operation = resource.operation.json();
            if (operation["type"] == "obs" && operation["operation"]["type"] == "status")
            {
                if (obs_ == nullptr)
                {
                    this->send_resource(resource.request_id,
                                        meowlive::desktop_runtime::obs::resource_status({}));
                    return;
                }
                obs_->execute(
                    meowlive::desktop_runtime::obs::Operation::Status, {}, *this,
                    [this, id = resource.request_id, epoch = connection_epoch_](
                        std::expected<meowlive::desktop_runtime::obs::Snapshot, ::QString> status)
                    {
                        if (epoch != connection_epoch_ || stage_ != Stage::Paired)
                        {
                            return;
                        }
                        if (status)
                        {
                            this->send_resource(
                                id, meowlive::desktop_runtime::obs::resource_status(*status));
                        }
                        else
                        {
                            this->send_resource_error(id, "obs_error",
                                                      status.error().toStdString());
                        }
                    });
                return;
            }
            if (avatar_ &&
                meowlive::desktop_runtime::avatar::query_resource(
                    *avatar_, operation,
                    [connection = ::QPointer<ServerConnection>(this), id = resource.request_id,
                     epoch = connection_epoch_](meowlive::protocol::DesktopResourceResult result)
                    {
                        if (connection && epoch == connection->connection_epoch_ &&
                            connection->stage_ == Stage::Paired)
                        {
                            connection->send_resource(id, std::move(result));
                        }
                    }))
            {
                return;
            }
            this->send_resource_error(resource.request_id, "resource_unsupported",
                                      "Desktop resource operation is unavailable");
        }

        void send_resource_error(const std::string& id, const char* code,
                                 const std::string& message)
        {
            ::Json::Value error(::Json::objectValue);
            error["type"] = "error";
            error["code"] = code;
            error["message"] = message;
            this->send_resource(id,
                                meowlive::protocol::DesktopResourceResult::decode(error).value());
        }

        void send_resource(const std::string& id, meowlive::protocol::DesktopResourceResult result)
        {
            const auto message = meowlive::protocol::encode_client_message(
                meowlive::protocol::ResourceResultMessage{id, std::move(result)});
            if (control_.sendTextMessage(::QString::fromUtf8(
                    message.data(), static_cast<::qsizetype>(message.size()))) <= 0)
            {
                this->retry(QStringLiteral("发送资源结果失败，正在重连"));
            }
        }

        void ping()
        {
            if (control_alive_.hasExpired(35'000) || audio_alive_.hasExpired(35'000))
            {
                this->retry(QStringLiteral("执行端心跳超时，正在重连"));
                return;
            }
            control_.ping();
            audio_.ping();
        }

        void retry(const ::QString& message)
        {
            if (stage_ == Stage::Idle || stage_ == Stage::Reconnecting)
            {
                return;
            }
            stage_ = Stage::Reconnecting;
            ++connection_epoch_;
            heartbeat_.stop();
            handshake_.stop();
            receipts_.stop();
            playback_.reset();
            status_(message);

            // Socket signals may be emitted while Qt is processing an incoming frame.
            ::QTimer::singleShot(0, this,
                                 [this]
                                 {
                                     audio_.abort();
                                     control_.abort();
                                     retry_.start();
                                 });
        }

        ::QUrl origin_;
        ::QByteArray token_;
        std::function<void(const ::QString&)> status_;
        meowlive::desktop_runtime::obs::Client* obs_{};
        meowlive::desktop_runtime::avatar::Presentation* avatar_{};
        unsigned connection_epoch_{};
        Stage stage_{Stage::Idle};
        ::QTimer handshake_;
        ::QTimer heartbeat_;
        ::QTimer retry_;
        ::QTimer receipts_;
        ::QElapsedTimer control_alive_;
        ::QElapsedTimer audio_alive_;
        ::QWebSocket control_;
        ::QWebSocket audio_;
        meowlive::desktop_runtime::audio::Playback playback_;
    };
}
