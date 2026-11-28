module;

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkProxy>
#include <QtWebSockets/QWebSocket>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.desktop_runtime.obs.client;

import meowlive.standard;
import meowlive.desktop_runtime.obs.settings;
import meowlive.protocol.resources;

export namespace meowlive::desktop_runtime::obs
{
    enum class Operation
    {
        Status,
        SetScene,
        StartRecording,
        StopRecording,
        StartStreaming,
        StopStreaming,
    };

    struct Snapshot
    {
        bool connected{};
        bool recording{};
        bool streaming{};
        ::QString current_scene;
        ::QStringList scenes;
    };
}

namespace
{
    using Result = std::expected<meowlive::desktop_runtime::obs::Snapshot, ::QString>;
    using Operation = meowlive::desktop_runtime::obs::Operation;

    bool valid_scene(const ::QString& name)
    {
        if (name.isEmpty() || name.toUcs4().size() > 256)
        {
            return false;
        }
        for (const auto character : name)
        {
            if (character.category() == ::QChar::Other_Control)
            {
                return false;
            }
        }
        return true;
    }

    class Exchange final : public ::QObject
    {
    public:
        Exchange(meowlive::desktop_runtime::obs::Settings settings, Operation operation,
                 ::QString scene, std::function<void(Result)> finish, ::QObject& owner)
            : ::QObject(&owner), settings_(std::move(settings)), operation_(operation),
              scene_(std::move(scene)), finish_(std::move(finish))
        {
            socket_.setProxy(::QNetworkProxy::NoProxy);
            socket_.setMaxAllowedIncomingFrameSize(256 * 1024);
            socket_.setMaxAllowedIncomingMessageSize(256 * 1024);
            timer_.setSingleShot(true);
            this->connect(&timer_, &::QTimer::timeout, this,
                          [this]
                          {
                              this->fail(QStringLiteral("OBS 操作超时"));
                          });
            this->connect(&socket_, &::QWebSocket::textMessageReceived, this,
                          [this](const ::QString& text)
                          {
                              this->receive(text);
                          });
            this->connect(&socket_, &::QWebSocket::binaryMessageReceived, this,
                          [this]
                          {
                              this->fail(QStringLiteral("OBS 返回了无效的二进制消息"));
                          });
            this->connect(&socket_, &::QWebSocket::errorOccurred, this,
                          [this]
                          {
                              this->fail(step_ == Step::Identified
                                             ? QStringLiteral("OBS 鉴权失败，连接已关闭")
                                             : QStringLiteral("OBS WebSocket 连接失败"));
                          });
            this->connect(&socket_, &::QWebSocket::disconnected, this,
                          [this]
                          {
                              this->fail(step_ == Step::Identified
                                             ? QStringLiteral("OBS 鉴权失败，连接已关闭")
                                             : QStringLiteral("OBS 连接已关闭"));
                          });
        }

        void start()
        {
            timer_.start(5'000);
            socket_.open(::QUrl(settings_.websocket_url));
        }

    private:
        enum class Step
        {
            Hello,
            Identified,
            Scenes,
            Recording,
            Streaming,
            Mutation,
        };

        void send(const ::QJsonObject& message)
        {
            const auto encoded = ::QJsonDocument(message).toJson(::QJsonDocument::Compact);
            if (socket_.sendTextMessage(::QString::fromUtf8(encoded)) <= 0)
            {
                this->fail(QStringLiteral("发送 OBS 请求失败"));
            }
        }

        void request(Step step, const ::QString& kind, const ::QJsonObject& data = {})
        {
            step_ = step;
            kind_ = kind;
            const ::QJsonObject request{
                {QStringLiteral("requestType"), kind},
                {QStringLiteral("requestId"), ::QString::number(++request_id_)},
                {QStringLiteral("requestData"), data}};
            this->send({{QStringLiteral("op"), 6}, {QStringLiteral("d"), request}});
        }

        void scenes()
        {
            this->request(Step::Scenes, QStringLiteral("GetSceneList"));
        }

        void receive(const ::QString& text)
        {
            if (!finish_)
            {
                return;
            }
            const auto message = ::QJsonDocument::fromJson(text.toUtf8()).object();
            const auto opcode = message.value(QStringLiteral("op")).toInt(-1);
            const auto data = message.value(QStringLiteral("d")).toObject();
            if (step_ == Step::Hello && opcode == 0)
            {
                this->identify(data);
            }
            else if (step_ == Step::Identified && opcode == 2 &&
                     data.value(QStringLiteral("negotiatedRpcVersion")).toInt() == 1)
            {
                this->scenes();
            }
            else if (step_ != Step::Hello && step_ != Step::Identified && opcode == 7)
            {
                this->response(data);
            }
            else if (opcode != 5 || step_ == Step::Hello || step_ == Step::Identified)
            {
                this->fail(QStringLiteral("OBS 返回了无效的协议消息"));
            }
        }

        void identify(const ::QJsonObject& hello)
        {
            if (hello.value(QStringLiteral("rpcVersion")).toInt() < 1)
            {
                this->fail(QStringLiteral("OBS RPC 版本无效"));
                return;
            }
            ::QJsonObject data{{QStringLiteral("rpcVersion"), 1},
                               {QStringLiteral("eventSubscriptions"), 0}};
            if (hello.contains(QStringLiteral("authentication")))
            {
                const auto auth = hello.value(QStringLiteral("authentication")).toObject();
                const auto salt = auth.value(QStringLiteral("salt")).toString();
                const auto challenge = auth.value(QStringLiteral("challenge")).toString();
                if (salt.isEmpty() || challenge.isEmpty() || settings_.password.isEmpty())
                {
                    this->fail(QStringLiteral("OBS 要求有效的 WebSocket 密码"));
                    return;
                }
                const auto secret = ::QCryptographicHash::hash(
                    settings_.password.toUtf8() + salt.toUtf8(), ::QCryptographicHash::Sha256);
                const auto authentication = ::QCryptographicHash::hash(
                    secret.toBase64() + challenge.toUtf8(), ::QCryptographicHash::Sha256);
                data.insert(QStringLiteral("authentication"),
                            ::QString::fromLatin1(authentication.toBase64()));
            }
            step_ = Step::Identified;
            this->send({{QStringLiteral("op"), 1}, {QStringLiteral("d"), data}});
        }

        void response(const ::QJsonObject& data)
        {
            if (data.value(QStringLiteral("requestType")).toString() != kind_ ||
                data.value(QStringLiteral("requestId")).toString() !=
                    ::QString::number(request_id_))
            {
                this->fail(QStringLiteral("OBS 响应与请求不匹配"));
                return;
            }
            const auto status = data.value(QStringLiteral("requestStatus")).toObject();
            const auto code = status.value(QStringLiteral("code")).toInt(-1);
            const auto accepted = status.value(QStringLiteral("result"));
            if (!accepted.isBool() || code < 0)
            {
                this->fail(QStringLiteral("OBS 请求状态无效"));
                return;
            }
            if (!accepted.toBool() || code != 100)
            {
                this->fail(QStringLiteral("OBS 请求 %1 失败（代码 %2）").arg(kind_).arg(code));
                return;
            }
            const auto result = data.value(QStringLiteral("responseData")).toObject();
            const auto active = result.value(QStringLiteral("outputActive"));
            switch (step_)
            {
            case Step::Scenes:
                if (this->read_scenes(result))
                {
                    this->request(Step::Recording, QStringLiteral("GetRecordStatus"));
                }
                return;
            case Step::Recording:
                if (active.isBool())
                {
                    snapshot_.recording = active.toBool();
                    this->request(Step::Streaming, QStringLiteral("GetStreamStatus"));
                    return;
                }
                break;
            case Step::Streaming:
                if (active.isBool())
                {
                    snapshot_.streaming = active.toBool();
                    this->apply();
                    return;
                }
                break;
            case Step::Mutation:
                mutation_submitted_ = true;
                this->retry_snapshot();
                return;
            default:
                break;
            }
            this->fail(QStringLiteral("OBS 输出状态无效"));
        }

        bool read_scenes(const ::QJsonObject& data)
        {
            const auto scene_list = data.value(QStringLiteral("scenes"));
            const auto current = data.value(QStringLiteral("currentProgramSceneName")).toString();
            ::QStringList names;
            if (scene_list.isArray() && scene_list.toArray().size() <= 256 && valid_scene(current))
            {
                const auto entries = scene_list.toArray();
                for (const auto& entry : entries)
                {
                    const auto scene =
                        entry.toObject().value(QStringLiteral("sceneName")).toString();
                    if (!valid_scene(scene))
                    {
                        break;
                    }
                    names.push_back(scene);
                }
                if (names.size() == entries.size())
                {
                    snapshot_.current_scene = current;
                    snapshot_.scenes = std::move(names);
                    return true;
                }
            }
            this->fail(QStringLiteral("OBS 场景列表无效"));
            return false;
        }

        void apply()
        {
            if (operation_ == Operation::SetScene)
            {
                if (!snapshot_.scenes.contains(scene_))
                {
                    this->fail(QStringLiteral("OBS 场景不存在"));
                    return;
                }
                if (scene_ != snapshot_.current_scene)
                {
                    if (mutation_submitted_)
                    {
                        this->retry_snapshot();
                    }
                    else
                    {
                        this->request(Step::Mutation, QStringLiteral("SetCurrentProgramScene"),
                                      {{QStringLiteral("sceneName"), scene_}});
                    }
                    return;
                }
            }
            ::QString action;
            switch (operation_)
            {
            case Operation::StartRecording:
                if (!snapshot_.recording)
                    action = QStringLiteral("StartRecord");
                break;
            case Operation::StopRecording:
                if (snapshot_.recording)
                    action = QStringLiteral("StopRecord");
                break;
            case Operation::StartStreaming:
                if (!snapshot_.streaming)
                    action = QStringLiteral("StartStream");
                break;
            case Operation::StopStreaming:
                if (snapshot_.streaming)
                    action = QStringLiteral("StopStream");
                break;
            default:
                break;
            }
            if (!action.isEmpty())
            {
                if (mutation_submitted_)
                {
                    this->retry_snapshot();
                }
                else
                {
                    this->request(Step::Mutation, action);
                }
                return;
            }
            snapshot_.connected = true;
            this->complete(std::move(snapshot_));
        }

        void retry_snapshot()
        {
            // OBS acknowledges output actions before the encoder changes its active state.
            ::QTimer::singleShot(125, this,
                                 [this]
                                 {
                                     if (finish_)
                                     {
                                         this->scenes();
                                     }
                                 });
        }

        void fail(const ::QString& error)
        {
            this->complete(std::unexpected(error));
        }

        void complete(Result result)
        {
            if (finish_)
            {
                timer_.stop();
                this->deleteLater();
                auto finish = std::exchange(finish_, {});
                finish(std::move(result));
            }
        }

        meowlive::desktop_runtime::obs::Settings settings_;
        Operation operation_;
        ::QString scene_;
        std::function<void(Result)> finish_;
        ::QWebSocket socket_;
        ::QTimer timer_;
        meowlive::desktop_runtime::obs::Snapshot snapshot_;
        Step step_{Step::Hello};
        unsigned request_id_{};
        ::QString kind_;
        bool mutation_submitted_{};
    };
}

export namespace meowlive::desktop_runtime::obs
{
    class Client final : public ::QObject
    {
    public:
        explicit Client(::QString settings_path) : path_(std::move(settings_path))
        {
        }

        std::expected<meowlive::desktop_runtime::obs::SettingsView, ::QString> settings() const
        {
            auto result = meowlive::desktop_runtime::obs::load_settings(path_);
            if (!result)
            {
                return std::unexpected(result.error());
            }
            return meowlive::desktop_runtime::obs::settings_view(*result);
        }

        std::expected<meowlive::desktop_runtime::obs::SettingsView, ::QString>
        save(bool enabled, const ::QString& url, const ::QString& password,
             bool clear_password) const
        {
            return meowlive::desktop_runtime::obs::save_settings(path_, enabled, url, password,
                                                                 clear_password);
        }

        void execute(
            meowlive::desktop_runtime::obs::Operation operation, const ::QString& scene,
            ::QObject& receiver,
            std::function<void(std::expected<meowlive::desktop_runtime::obs::Snapshot, ::QString>)>
                finish)
        {
            if (busy_)
            {
                finish(std::unexpected(QStringLiteral("OBS 正在处理其他操作")));
                return;
            }
            auto settings = meowlive::desktop_runtime::obs::load_settings(path_);
            if (!settings)
            {
                finish(std::unexpected(settings.error()));
                return;
            }
            if (!settings->enabled)
            {
                if (operation == meowlive::desktop_runtime::obs::Operation::Status)
                {
                    finish(meowlive::desktop_runtime::obs::Snapshot{});
                }
                else
                {
                    finish(std::unexpected(QStringLiteral("OBS 接入未启用")));
                }
                return;
            }

            busy_ = true;
            auto* exchange = new Exchange(
                std::move(*settings), operation, scene,
                [this, target = ::QPointer<::QObject>(&receiver),
                 finish = std::move(finish)](Result result)
                {
                    busy_ = false;
                    if (target)
                    {
                        finish(std::move(result));
                    }
                },
                *this);
            exchange->start();
        }

    private:
        ::QString path_;
        bool busy_{};
    };

    meowlive::protocol::DesktopResourceResult
    resource_status(const meowlive::desktop_runtime::obs::Snapshot& status)
    {
        ::Json::Value result(::Json::objectValue);
        result["type"] = "obs";
        auto& snapshot = result["snapshot"];
        snapshot["connected"] = status.connected;
        snapshot["recording"] = status.recording;
        snapshot["current_scene"] = status.current_scene.toStdString();
        snapshot["scenes"] = ::Json::Value(::Json::arrayValue);
        for (const auto& scene : status.scenes)
        {
            snapshot["scenes"].append(scene.toStdString());
        }
        return meowlive::protocol::DesktopResourceResult::decode(result).value();
    }
}
