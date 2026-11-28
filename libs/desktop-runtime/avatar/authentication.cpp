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
    void Presentation::authenticate()
    {
        if (!token_.isEmpty())
        {
            this->authenticate_token();
            return;
        }
        token_requested_ = true;
        status_(QStringLiteral("请在 VTube Studio 确认授权"), false);
        this->request(
            QStringLiteral("AuthenticationToken"),
            meowlive::desktop_runtime::avatar::plugin_identity(),
            [this](const ::QJsonObject& response)
            {
                token_ = response.value(QStringLiteral("authenticationToken")).toString();
                if (!meowlive::desktop_runtime::avatar::valid_token(token_))
                {
                    this->fail(QStringLiteral("VTube Studio 返回了无效授权"));
                    return;
                }
                this->authenticate_token();
            },
            60'000);
    }

    void Presentation::authenticate_token()
    {
        auto data = meowlive::desktop_runtime::avatar::plugin_identity();
        data[QStringLiteral("authenticationToken")] = token_;
        this->request(QStringLiteral("Authentication"), data,
                      [this](const ::QJsonObject& response)
                      {
                          if (!response.value(QStringLiteral("authenticated")).toBool())
                          {
                              this->fail(QStringLiteral(
                                  "VTube Studio 拒绝了已有授权，请在授权页面清除本地令牌后重试"));
                              return;
                          }
                          const bool saved =
                              !token_requested_ ||
                              meowlive::desktop_runtime::avatar::save_token(path_, token_);
                          token_requested_ = false;
                          this->create_mouth_parameter(saved);
                      });
    }

    void Presentation::create_mouth_parameter(bool token_saved)
    {
        this->request(QStringLiteral("ParameterCreation"), this->parameter_data(mouth_parameter_),
                      [this, token_saved](const ::QJsonObject& response)
                      {
                          if (response.value(QStringLiteral("parameterName")) != mouth_parameter_)
                          {
                              this->fail(QStringLiteral("VTube Studio 口型参数未就绪"));
                              return;
                          }
                          ready_ = true;
                          this->send_mouth(0);
                          status_(token_saved
                                      ? QStringLiteral("VTube Studio 已连接")
                                      : QStringLiteral("VTube Studio 已连接，无法保存本地授权"),
                                  true);
                      });
    }
}
