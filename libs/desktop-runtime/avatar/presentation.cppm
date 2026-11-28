module;

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

export module meowlive.desktop_runtime.avatar.presentation;

import meowlive.desktop_runtime.avatar.protocol;
import meowlive.desktop_runtime.avatar.token;

export namespace meowlive::desktop_runtime::avatar
{
    class Presentation final : public ::QObject
    {
    public:
        using ResponseHandler = std::function<void(const ::QJsonObject&)>;
        using FailureHandler = std::function<void(const ::QString&)>;
        using StatusHandler = std::function<void(const ::QString&, bool)>;

        explicit Presentation(meowlive::desktop_runtime::avatar::Presentation::StatusHandler status)
            : status_(std::move(status))
        {
            socket_.setProxy(::QNetworkProxy::NoProxy);
            socket_.setMaxAllowedIncomingFrameSize(65'536);
            socket_.setMaxAllowedIncomingMessageSize(65'536);
            timeout_.setSingleShot(true);
            reconnect_.setSingleShot(true);
            this->connect(&socket_, &::QWebSocket::connected, this,
                          [this]
                          {
                              this->authenticate();
                          });
            this->connect(&socket_, &::QWebSocket::disconnected, this,
                          [this]
                          {
                              this->disconnected();
                          });
            this->connect(&socket_, &::QWebSocket::textMessageReceived, this,
                          [this](const ::QString& message)
                          {
                              this->receive(message);
                          });
            this->connect(&socket_, &::QWebSocket::errorOccurred, this,
                          [this](::QAbstractSocket::SocketError)
                          {
                              this->disconnected();
                          });
            this->connect(&timeout_, &::QTimer::timeout, this,
                          [this]
                          {
                              if (!ready_)
                              {
                                  this->fail(token_requested_
                                                 ? QStringLiteral("VTube Studio 授权等待超时")
                                                 : QStringLiteral("连接 VTube Studio 超时"));
                              }
                              else
                              {
                                  this->cancel_request(QStringLiteral("VTube Studio 操作超时"));
                              }
                          });
            this->connect(&reconnect_, &::QTimer::timeout, this,
                          [this]
                          {
                              this->open();
                          });
        }

        ~Presentation() override
        {
            this->stop();
        }

        bool connected() const noexcept
        {
            return ready_;
        }

        void start(const ::QString& address, bool renew = false)
        {
            const ::QUrl endpoint(address, ::QUrl::StrictMode);
            if (!meowlive::desktop_runtime::avatar::valid_endpoint(endpoint))
            {
                status_(QStringLiteral("请输入本机 IP 和端口，例如 ws://127.0.0.1:8001"), ready_);
                return;
            }
            this->stop();
            endpoint_ = endpoint;
            path_ = meowlive::desktop_runtime::avatar::token_path();
            requested_ = true;
            token_.clear();
            if (renew)
            {
                this->open();
                return;
            }
            const auto saved = meowlive::desktop_runtime::avatar::load_token(path_);
            if (saved)
            {
                token_ = *saved;
            }
            else
            {
                this->fail(saved.error());
                return;
            }
            this->open();
        }

        void stop()
        {
            requested_ = false;
            token_requested_ = false;
            reconnect_.stop();
            if (ready_)
            {
                this->write(QStringLiteral("InjectParameterDataRequest"),
                            meowlive::desktop_runtime::avatar::mouth_input(0, mouth_parameter_));
            }
            ready_ = false;
            this->reset();
            if (socket_.state() == ::QAbstractSocket::ConnectedState)
            {
                socket_.close();
            }
            else
            {
                socket_.abort();
            }
        }

        void
        refresh_models(meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                       meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure = {})
        {
            this->operate(QStringLiteral("AvailableModels"), {}, std::move(handler),
                          std::move(failure));
        }

        void select_model(const ::QString& id,
                          meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler)
        {
            this->load_model(id, mouth_parameter_, std::move(handler), {});
        }

        void load_model(const ::QString& id, const ::QString& parameter,
                        meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                        meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure)
        {
            if (id.isEmpty() || id.size() > 1'024 || parameter.size() < 4 || parameter.size() > 32)
            {
                this->report_failure(QStringLiteral("模型或口型参数无效"), failure);
                return;
            }
            this->operate(
                QStringLiteral("ModelLoad"), {{QStringLiteral("modelID"), id}},
                [this, id, parameter, handler = std::move(handler),
                 failure](const ::QJsonObject& response)
                {
                    if (response.value(QStringLiteral("modelID")) != id)
                    {
                        this->report_failure(QStringLiteral("VTube Studio 返回了不同的模型"),
                                             failure);
                        return;
                    }
                    if (mouth_parameter_ == parameter)
                    {
                        handler(response);
                        return;
                    }
                    this->operate(
                        QStringLiteral("ParameterCreation"), this->parameter_data(parameter),
                        [this, parameter, handler, response, failure](const ::QJsonObject& result)
                        {
                            if (result.value(QStringLiteral("parameterName")) != parameter)
                            {
                                this->report_failure(QStringLiteral("VTube Studio 口型参数未就绪"),
                                                     failure);
                                return;
                            }
                            this->write(QStringLiteral("InjectParameterDataRequest"),
                                        meowlive::desktop_runtime::avatar::mouth_input(
                                            0, mouth_parameter_));
                            mouth_parameter_ = parameter;
                            this->send_mouth(0);
                            handler(response);
                        },
                        failure);
                },
                failure);
        }

        void
        list_hotkeys(const ::QString& model_id,
                     meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                     meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure = {})
        {
            this->operate(QStringLiteral("HotkeysInCurrentModel"),
                          {{QStringLiteral("modelID"), model_id}}, std::move(handler),
                          std::move(failure));
        }

        void preview_hotkey(const ::QString& model_id, const ::QString& hotkey_id)
        {
            this->trigger_hotkey(model_id, hotkey_id,
                                 [this](const ::QJsonObject&)
                                 {
                                     status_(QStringLiteral("已在 VTube Studio 预览"), true);
                                 },
                                 {});
        }

        void
        trigger_hotkey(const ::QString& model_id, const ::QString& hotkey_id,
                       meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                       meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure)
        {
            this->operate(
                QStringLiteral("CurrentModel"), {},
                [this, model_id, hotkey_id, handler = std::move(handler),
                 failure](const ::QJsonObject& response)
                {
                    if (!response.value(QStringLiteral("modelLoaded")).toBool() ||
                        response.value(QStringLiteral("modelID")) != model_id)
                    {
                        this->report_failure(QStringLiteral("请先在 VTube Studio 加载选中的模型"),
                                             failure);
                        return;
                    }
                    this->operate(
                        QStringLiteral("HotkeyTrigger"), {{QStringLiteral("hotkeyID"), hotkey_id}},
                        [this, hotkey_id, handler, failure](const ::QJsonObject& response)
                        {
                            if (response.value(QStringLiteral("hotkeyID")) == hotkey_id)
                            {
                                handler(response);
                            }
                            else
                            {
                                this->report_failure(
                                    QStringLiteral("VTube Studio 返回了不同的快捷键"), failure);
                            }
                        },
                        failure);
                },
                failure);
        }

        void set_mouth_level(double rms)
        {
            if (!ready_)
            {
                return;
            }
            const auto level = std::isfinite(rms) ? std::clamp((rms - 0.01) * 4.0, 0.0, 1.0) : 0.0;
            const bool close_mouth = level == 0 && last_level_ != 0;
            if (!mouth_id_.isEmpty() && !close_mouth)
            {
                if (mouth_time_.elapsed() > 1000)
                {
                    socket_.abort();
                }
                return;
            }
            if (!close_mouth && level == last_level_ && mouth_time_.elapsed() < 500)
            {
                return;
            }
            this->send_mouth(level);
        }

    private:
        void open()
        {
            if (!requested_)
            {
                return;
            }
            if (socket_.state() != ::QAbstractSocket::UnconnectedState)
            {
                socket_.abort();
            }
            reconnect_.stop();
            status_(QStringLiteral("正在连接 VTube Studio"), false);
            timeout_.start(5000);
            socket_.open(endpoint_);
        }

        void reset()
        {
            if (failure_)
            {
                this->cancel_request(QStringLiteral("VTube Studio 连接已断开"));
            }
            else
            {
                this->clear_request();
            }
            mouth_id_.clear();
            last_level_ = 0;
        }

        void clear_request()
        {
            timeout_.stop();
            request_id_.clear();
            response_type_.clear();
            request_ = {};
            failure_ = {};
        }

        void report_failure(
            const ::QString& message,
            const meowlive::desktop_runtime::avatar::Presentation::FailureHandler& failure)
        {
            if (failure)
            {
                failure(message);
            }
            else
            {
                status_(message, ready_);
            }
        }

        void cancel_request(const ::QString& message)
        {
            auto failure = std::move(failure_);
            this->clear_request();
            this->report_failure(message, failure);
        }

        void disconnected()
        {
            ready_ = false;
            this->reset();
            if (!requested_)
            {
                return;
            }
            if (token_requested_ && token_.isEmpty())
            {
                this->fail(QStringLiteral("连接已断开，请重新发起 VTube Studio 授权"));
            }
            else if (!reconnect_.isActive())
            {
                status_(QStringLiteral("VTube Studio 已断开，正在等待重连"), false);
                reconnect_.start(2000);
            }
        }

        void fail(const ::QString& message)
        {
            requested_ = false;
            token_requested_ = false;
            reconnect_.stop();
            ready_ = false;
            this->reset();
            socket_.abort();
            status_(message, false);
        }

        ::QString write(const ::QString& type, const ::QJsonObject& data)
        {
            const auto id = QStringLiteral("meow-") + ::QString::number(++next_id_);
            const auto text = meowlive::desktop_runtime::avatar::encode_request(type, id, data);
            if (socket_.sendTextMessage(text) <= 0)
            {
                return {};
            }
            return id;
        }

        void request(const ::QString& action, const ::QJsonObject& data,
                     meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                     int timeout = 1000)
        {
            const auto id = this->write(action + QStringLiteral("Request"), data);
            if (id.isEmpty())
            {
                socket_.abort();
                return;
            }
            request_id_ = id;
            response_type_ = action + QStringLiteral("Response");
            request_ = std::move(handler);
            timeout_.start(timeout);
        }

        void operate(const ::QString& action, const ::QJsonObject& data,
                     meowlive::desktop_runtime::avatar::Presentation::ResponseHandler handler,
                     meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure = {})
        {
            if (ready_ && !request_)
            {
                this->request(action, data, std::move(handler));
                if (request_)
                {
                    failure_ = std::move(failure);
                    return;
                }
            }
            if (failure)
            {
                this->report_failure(ready_ ? QStringLiteral("VTube Studio 正在处理另一项操作")
                                            : QStringLiteral("VTube Studio 尚未连接"),
                                     failure);
            }
        }

        void authenticate();

        void authenticate_token();

        ::QJsonObject parameter_data(const ::QString& parameter) const
        {
            return {{QStringLiteral("parameterName"), parameter},
                    {QStringLiteral("explanation"),
                     QStringLiteral("MeowLive2D playback mouth opening")},
                    {QStringLiteral("min"), 0.0},
                    {QStringLiteral("max"), 1.0},
                    {QStringLiteral("defaultValue"), 0.0}};
        }

        void create_mouth_parameter(bool token_saved);

        void receive(const ::QString& text);

        void send_mouth(double level);

        meowlive::desktop_runtime::avatar::Presentation::StatusHandler status_;
        meowlive::desktop_runtime::avatar::Presentation::ResponseHandler request_;
        meowlive::desktop_runtime::avatar::Presentation::FailureHandler failure_;
        ::QWebSocket socket_;
        ::QTimer timeout_;
        ::QTimer reconnect_;
        ::QElapsedTimer mouth_time_;
        ::QUrl endpoint_;
        ::QString path_;
        ::QString token_;
        ::QString request_id_;
        ::QString response_type_;
        ::QString mouth_id_;
        ::QString mouth_parameter_{meowlive::desktop_runtime::avatar::mouth_parameter};
        ::qulonglong next_id_{};
        double last_level_{};
        bool requested_{};
        bool ready_{};
        bool token_requested_{};
    };
}
