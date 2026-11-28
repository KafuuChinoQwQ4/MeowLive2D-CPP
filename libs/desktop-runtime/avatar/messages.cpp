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

module meowlive.desktop_runtime.avatar.presentation;

import meowlive.desktop_runtime.avatar.protocol;
import meowlive.desktop_runtime.avatar.token;

namespace meowlive::desktop_runtime::avatar
{
    void Presentation::receive(const ::QString& text)
    {
        const auto response = meowlive::desktop_runtime::avatar::decode_reply(text);
        if (!response || response->id.isEmpty())
        {
            return;
        }
        if (response->id == mouth_id_)
        {
            mouth_id_.clear();
            if (response->type != QStringLiteral("InjectParameterDataResponse"))
            {
                this->fail(QStringLiteral("VTube Studio 拒绝了口型参数"));
            }
        }
        else if (request_ && response->id == request_id_)
        {
            const auto expected = response->type == response_type_;
            auto handler = std::move(request_);
            auto failure = std::move(failure_);
            this->clear_request();
            if (expected)
            {
                handler(response->data);
            }
            else if (ready_)
            {
                this->report_failure(QStringLiteral("VTube Studio 拒绝了该操作"), failure);
            }
            else
            {
                this->fail(QStringLiteral("VTube Studio 授权或口型参数设置失败"));
            }
        }
    }

    void Presentation::send_mouth(double level)
    {
        mouth_id_ =
            this->write(QStringLiteral("InjectParameterDataRequest"),
                        meowlive::desktop_runtime::avatar::mouth_input(level, mouth_parameter_));
        if (mouth_id_.isEmpty())
        {
            socket_.abort();
            return;
        }
        last_level_ = level;
        mouth_time_.start();
    }
}
