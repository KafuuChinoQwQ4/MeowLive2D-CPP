module;

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <expected>
#include <functional>
#include <utility>

export module meowlive.desktop.services.update_readiness;

import meowlive.desktop.services.server_client;
import meowlive.protocol.control;

namespace
{
    using Complete = std::function<void(const ::QString&)>;

    ::QString unknown_server_status()
    {
        return QStringLiteral("无法确认主服务状态，请检查连接和管理员会话");
    }

    void check_speech(meowlive::desktop::services::ServerClient& server, ::QObject& receiver,
                      Complete complete)
    {
        server.get_json(
            QStringLiteral("/api/status"), receiver,
            [complete = std::move(complete)](const ::QJsonDocument& document,
                                             const ::QString& error)
            {
                if (!error.isEmpty() || !document.isObject())
                {
                    complete(unknown_server_status());
                    return;
                }
                const auto speeches = document.object().value(QStringLiteral("speeches"));
                if (!speeches.isArray())
                {
                    complete(QStringLiteral("无法确认播报是否已结束"));
                    return;
                }
                for (const auto& entry : speeches.toArray())
                {
                    const auto phase = entry.toObject().value(QStringLiteral("status")).toString();
                    if (phase != QStringLiteral("completed") &&
                        phase != QStringLiteral("cancelled") && phase != QStringLiteral("failed") &&
                        phase != QStringLiteral("unknown"))
                    {
                        complete(QStringLiteral("请等待播报结束"));
                        return;
                    }
                }
                complete({});
            });
    }

    void check_training(meowlive::desktop::services::ServerClient& server, ::QObject& receiver,
                        Complete complete)
    {
        server.get_training_status(
            receiver,
            [&server, &receiver, complete = std::move(complete)](
                std::expected<meowlive::desktop::services::TrainingStatus, ::QString>
                    status) mutable
            {
                if (!status)
                {
                    complete(status.error());
                }
                else if (status->busy)
                {
                    complete(QStringLiteral("请等待语音训练结束"));
                }
                else
                {
                    check_speech(server, receiver, std::move(complete));
                }
            });
    }

    void check_live(meowlive::desktop::services::ServerClient& server, ::QObject& receiver,
                    Complete complete)
    {
        server.get_json(
            QStringLiteral("/api/live"), receiver,
            [&server, &receiver, complete = std::move(complete)](const ::QJsonDocument& document,
                                                                 const ::QString& error) mutable
            {
                if (!error.isEmpty() || !document.isObject())
                {
                    complete(unknown_server_status());
                    return;
                }
                const auto phase = document.object().value(QStringLiteral("phase")).toString();
                if (phase == QStringLiteral("disabled") ||
                    phase == QStringLiteral("disconnected") || phase == QStringLiteral("failed"))
                {
                    check_training(server, receiver, std::move(complete));
                }
                else
                {
                    complete(QStringLiteral("请先断开直播连接"));
                }
            });
    }
}

export namespace meowlive::desktop::services
{
    void check_update_readiness(meowlive::desktop::services::ServerClient& server,
                                ::QObject& receiver, std::function<void(const ::QString&)> complete)
    {
        const auto path = meowlive::protocol::health_path;
        server.get_json(
            ::QString::fromLatin1(path.data(), static_cast<::qsizetype>(path.size())), receiver,
            [&server, &receiver, complete = std::move(complete)](const ::QJsonDocument& document,
                                                                 const ::QString& error) mutable
            {
                if (!error.isEmpty() || !document.isObject())
                {
                    complete(unknown_server_status());
                    return;
                }
                const auto health = document.object();
                if (health.value(QStringLiteral("service")) != QStringLiteral("meowlive") ||
                    health.value(QStringLiteral("protocol_version")).toInt() !=
                        meowlive::protocol::version)
                {
                    complete(QStringLiteral("主服务健康检查失败"));
                    return;
                }
                check_live(server, receiver, std::move(complete));
            });
    }
}
