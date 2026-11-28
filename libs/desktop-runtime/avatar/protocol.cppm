module;

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <optional>

export module meowlive.desktop_runtime.avatar.protocol;

export namespace meowlive::desktop_runtime::avatar
{
    inline const ::QString mouth_parameter = QStringLiteral("MeowMouthOpen");

    bool valid_endpoint(const ::QUrl& url)
    {
        return url.isValid() && url.scheme() == QStringLiteral("ws") &&
               ::QHostAddress(url.host()).isLoopback() && url.userInfo().isEmpty() &&
               !url.hasQuery() && !url.hasFragment() && url.port() > 0 &&
               (url.path().isEmpty() || url.path() == QStringLiteral("/"));
    }

    ::QString encode_request(const ::QString& type, const ::QString& id, const ::QJsonObject& data)
    {
        const ::QJsonObject message{
            {QStringLiteral("apiName"), QStringLiteral("VTubeStudioPublicAPI")},
            {QStringLiteral("apiVersion"), QStringLiteral("1.0")},
            {QStringLiteral("requestID"), id},
            {QStringLiteral("messageType"), type},
            {QStringLiteral("data"), data},
        };
        return ::QString::fromUtf8(::QJsonDocument(message).toJson(::QJsonDocument::Compact));
    }

    struct Reply
    {
        ::QString id;
        ::QString type;
        ::QJsonObject data;
    };

    std::optional<meowlive::desktop_runtime::avatar::Reply> decode_reply(const ::QString& text)
    {
        const auto message = ::QJsonDocument::fromJson(text.toUtf8()).object();
        if (message.value(QStringLiteral("apiName")) != QStringLiteral("VTubeStudioPublicAPI") ||
            message.value(QStringLiteral("apiVersion")) != QStringLiteral("1.0"))
        {
            return std::nullopt;
        }
        return meowlive::desktop_runtime::avatar::Reply{
            message.value(QStringLiteral("requestID")).toString(),
            message.value(QStringLiteral("messageType")).toString(),
            message.value(QStringLiteral("data")).toObject()};
    }

    ::QJsonObject plugin_identity()
    {
        return {{QStringLiteral("pluginName"), QStringLiteral("MeowLive2D")},
                {QStringLiteral("pluginDeveloper"), QStringLiteral("MeowLive2D")}};
    }

    ::QJsonObject
    mouth_input(double level,
                const ::QString& parameter = meowlive::desktop_runtime::avatar::mouth_parameter)
    {
        const ::QJsonObject value{{QStringLiteral("id"), parameter},
                                  {QStringLiteral("value"), level}};
        return {{QStringLiteral("mode"), QStringLiteral("set")},
                {QStringLiteral("parameterValues"), ::QJsonArray{value}}};
    }
}
