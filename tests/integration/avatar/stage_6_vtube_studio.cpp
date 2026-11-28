#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>
#include <json/json.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

import meowlive.desktop_runtime.avatar.presentation;
import meowlive.desktop_runtime.avatar.resources;
import meowlive.desktop_runtime.avatar.token;
import meowlive.protocol.resources;

namespace
{
    namespace avatar = meowlive::desktop_runtime::avatar;

    template <class Predicate> void require_event(Predicate completed, const char* message)
    {
        ::QElapsedTimer timer;
        timer.start();
        while (!completed() && timer.elapsed() < 4500)
        {
            ::QCoreApplication::processEvents(::QEventLoop::AllEvents, 10);
            ::QThread::msleep(5);
        }
        if (!completed())
        {
            throw std::runtime_error(message);
        }
    }

    class VTubeStudio
    {
    public:
        VTubeStudio()
            : server_(QStringLiteral("VTube Studio fixture"), ::QWebSocketServer::NonSecureMode)
        {
            if (!server_.listen(::QHostAddress::LocalHost, 0))
            {
                throw std::runtime_error("could not start fixture");
            }
            ::QObject::connect(&server_, &::QWebSocketServer::newConnection, &server_,
                               [this]
                               {
                                   auto* client = server_.nextPendingConnection();
                                   socket_ = client;
                                   ::QObject::connect(client, &::QWebSocket::disconnected, client,
                                                      &::QObject::deleteLater);
                                   ::QObject::connect(client, &::QWebSocket::textMessageReceived,
                                                      client,
                                                      [this, client](const ::QString& text)
                                                      {
                                                          this->reply(*client, text);
                                                      });
                               });
        }

        ::QString url() const
        {
            return QStringLiteral("ws://127.0.0.1:") + ::QString::number(server_.serverPort());
        }

        void drop()
        {
            if (socket_)
            {
                socket_->abort();
            }
        }

        std::vector<double> mouth;
        int token_requests{};
        int sessions{};
        int previews{};
        bool custom_mouth{};

    private:
        void reply(::QWebSocket& socket, const ::QString& text)
        {
            auto document = ::QJsonDocument::fromJson(text.toUtf8()).object();
            const auto type = document.value(QStringLiteral("messageType")).toString();
            const auto data = document.value(QStringLiteral("data")).toObject();
            if (!type.endsWith(QStringLiteral("Request")) ||
                document.value(QStringLiteral("apiName")) != QStringLiteral("VTubeStudioPublicAPI"))
            {
                return;
            }
            auto response_type = type.left(type.size() - 7) + QStringLiteral("Response");
            ::QJsonObject response;
            if (type == QStringLiteral("AuthenticationTokenRequest"))
            {
                ++token_requests;
                response[QStringLiteral("authenticationToken")] = QStringLiteral("vtube-fixture");
            }
            else if (type == QStringLiteral("AuthenticationRequest") &&
                     data.value(QStringLiteral("authenticationToken")) ==
                         QStringLiteral("vtube-fixture") &&
                     data.value(QStringLiteral("pluginName")) == QStringLiteral("MeowLive2D") &&
                     data.value(QStringLiteral("pluginDeveloper")) == QStringLiteral("MeowLive2D"))
            {
                ++sessions;
                response[QStringLiteral("authenticated")] = true;
            }
            else if (type == QStringLiteral("ParameterCreationRequest"))
            {
                response = data;
            }
            else if (type == QStringLiteral("InjectParameterDataRequest"))
            {
                const auto parameter =
                    data.value(QStringLiteral("parameterValues")).toArray().first().toObject();
                if (parameter.value(QStringLiteral("id")) == QStringLiteral("MeowMouthOpen") &&
                    data.value(QStringLiteral("mode")) == QStringLiteral("set"))
                {
                    mouth.push_back(parameter.value(QStringLiteral("value")).toDouble(-1));
                }
                custom_mouth |=
                    parameter.value(QStringLiteral("id")) == QStringLiteral("TestMouth");
            }
            else if (type == QStringLiteral("AvailableModelsRequest"))
            {
                response[QStringLiteral("availableModels")] = ::QJsonArray{
                    ::QJsonObject{{QStringLiteral("modelID"), QStringLiteral("model-fixture")},
                                  {QStringLiteral("modelName"), QStringLiteral("Fixture")},
                                  {QStringLiteral("modelLoaded"), active_}}};
            }
            else if (type == QStringLiteral("ModelLoadRequest") &&
                     data.value(QStringLiteral("modelID")) == QStringLiteral("model-fixture"))
            {
                active_ = true;
                response[QStringLiteral("modelID")] = QStringLiteral("model-fixture");
            }
            else if (type == QStringLiteral("CurrentModelRequest"))
            {
                response[QStringLiteral("modelLoaded")] = active_;
                response[QStringLiteral("modelID")] = QStringLiteral("model-fixture");
            }
            else if (type == QStringLiteral("HotkeysInCurrentModelRequest"))
            {
                response[QStringLiteral("modelID")] = QStringLiteral("model-fixture");
                response[QStringLiteral("availableHotkeys")] = ::QJsonArray{
                    ::QJsonObject{{QStringLiteral("hotkeyID"), QStringLiteral("preview-fixture")},
                                  {QStringLiteral("name"), QStringLiteral("Preview")}}};
            }
            else if (type == QStringLiteral("HotkeyTriggerRequest") &&
                     data.value(QStringLiteral("hotkeyID")) == QStringLiteral("preview-fixture"))
            {
                ++previews;
                response = data;
            }
            else
            {
                response_type = QStringLiteral("APIError");
            }
            document[QStringLiteral("messageType")] = response_type;
            document[QStringLiteral("data")] = response;
            socket.sendTextMessage(
                ::QString::fromUtf8(::QJsonDocument(document).toJson(::QJsonDocument::Compact)));
        }

        ::QWebSocketServer server_;
        ::QPointer<::QWebSocket> socket_;
        bool active_{};
    };

    void presentation()
    {
        ::QTemporaryDir directory;
        if (!directory.isValid() || !::qputenv("MEOWLIVE_DATA_DIR", directory.path().toUtf8()))
        {
            throw std::runtime_error("temporary user directory");
        }
        VTubeStudio studio;
        avatar::Presentation view(
            [](const ::QString&, bool)
            {
            });
        view.start(studio.url());
        require_event(
            [&]
            {
                return view.connected() && !studio.mouth.empty();
            },
            "authorize and reset mouth");
        if (studio.token_requests != 1 || studio.sessions != 1 || studio.mouth.front() != 0 ||
            avatar::load_token(avatar::token_path()) != QStringLiteral("vtube-fixture"))
        {
            throw std::runtime_error("first authorization persists the token");
        }

        bool models_observed{};
        view.refresh_models(
            [&](const ::QJsonObject& data)
            {
                models_observed =
                    data.value(QStringLiteral("availableModels")).toArray().size() == 1;
            });
        require_event(
            [&]
            {
                return models_observed;
            },
            "model list");
        bool selected{};
        view.select_model(QStringLiteral("model-fixture"),
                          [&](const ::QJsonObject&)
                          {
                              selected = true;
                          });
        require_event(
            [&]
            {
                return selected;
            },
            "select model");
        view.preview_hotkey(QStringLiteral("model-fixture"), QStringLiteral("preview-fixture"));
        require_event(
            [&]
            {
                return studio.previews == 1;
            },
            "preview active model hotkey");

        require_event(
            [&]
            {
                view.set_mouth_level(0.2);
                return studio.mouth.back() > 0.5;
            },
            "playback opens mouth");
        view.set_mouth_level(0);
        require_event(
            [&]
            {
                return studio.mouth.back() == 0;
            },
            "silence closes mouth");

        studio.drop();
        require_event(
            [&]
            {
                return !view.connected();
            },
            "socket loses ready state");
        require_event(
            [&]
            {
                return view.connected() && studio.sessions == 2;
            },
            "reconnect");
        view.stop();
        require_event(
            [&]
            {
                return studio.mouth.back() == 0;
            },
            "explicit stop closes mouth");

        avatar::Presentation reopened(
            [](const ::QString&, bool)
            {
            });
        reopened.start(studio.url());
        require_event(
            [&]
            {
                return reopened.connected() && studio.sessions == 3;
            },
            "subsequent process uses the token");
        if (studio.token_requests != 1)
        {
            throw std::runtime_error("reconnect requested a fresh token");
        }

        const auto resource = [&reopened](const ::Json::Value& input)
        {
            auto result = std::make_shared<::Json::Value>();
            if (!meowlive::desktop_runtime::avatar::query_resource(
                    reopened, input,
                    [result](meowlive::protocol::DesktopResourceResult response)
                    {
                        *result = response.json();
                    }))
            {
                throw std::runtime_error("unsupported avatar operation");
            }
            require_event(
                [result]
                {
                    return result->isObject();
                },
                "avatar resource response");
            return *result;
        };

        ::Json::Value input(::Json::objectValue);
        input["type"] = "list_models";
        const auto models = resource(input);
        if (models["type"] != "models" || models["models"][0u]["id"] != "model-fixture")
        {
            throw std::runtime_error("avatar resource model list");
        }
        input["type"] = "load_model";
        input["model_id"] = "model-fixture";
        input["mouth_parameter"] = "TestMouth";
        if (resource(input)["type"] != "model_loaded")
        {
            throw std::runtime_error("avatar resource model load");
        }
        require_event(
            [&studio]
            {
                return studio.custom_mouth;
            },
            "select custom mouth parameter");
        input["type"] = "list_hotkeys";
        if (resource(input)["hotkeys"][0u]["id"] != "preview-fixture")
        {
            throw std::runtime_error("avatar resource hotkey list");
        }
        input["type"] = "trigger_hotkey";
        input["hotkey_id"] = "missing-primary";
        input["fallback_hotkey_id"] = "preview-fixture";
        if (resource(input)["hotkey_id"] != "preview-fixture" || studio.previews != 2)
        {
            throw std::runtime_error("avatar resource fallback hotkey");
        }
        reopened.stop();
        if (resource(input)["type"] != "error")
        {
            throw std::runtime_error("avatar resource disconnected error");
        }
    }
}

int main(int argc, char* argv[])
{
    ::QCoreApplication app(argc, argv);
    try
    {
        presentation();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
