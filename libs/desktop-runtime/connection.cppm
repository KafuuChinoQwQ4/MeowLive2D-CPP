module;

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkRequest>
#include <QtWebSockets/QWebSocket>

#if defined(_WIN32)
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#endif

export module meowlive.desktop_runtime.connection;

#if !defined(_WIN32)
import std;
#endif
import meowlive.protocol.audio;
import meowlive.protocol.control;

export namespace meowlive::desktop_runtime
{
    class ServerConnection final : public ::QObject
    {
    public:
        ServerConnection(::QUrl origin, ::QByteArray token,
                         std::function<void(const ::QString&)> status)
            : origin_(std::move(origin)), token_(std::move(token)), status_(std::move(status))
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

            this->connect(&retry_, &::QTimer::timeout, this, &ServerConnection::connect_control);
            this->connect(&handshake_, &::QTimer::timeout, this,
                          [this]
                          {
                              this->retry(QStringLiteral("执行端配对超时，正在重连"));
                          });
            this->connect(&heartbeat_, &::QTimer::timeout, this, &ServerConnection::ping);
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
            if (!meowlive::protocol::decode_audio_chunk(bytes))
            {
                this->retry(QStringLiteral("音频帧无效，正在重连"));
                return;
            }
            audio_alive_.start();
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
            heartbeat_.stop();
            handshake_.stop();
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
        Stage stage_{Stage::Idle};
        ::QTimer handshake_;
        ::QTimer heartbeat_;
        ::QTimer retry_;
        ::QElapsedTimer control_alive_;
        ::QElapsedTimer audio_alive_;
        ::QWebSocket control_;
        ::QWebSocket audio_;
    };
}
