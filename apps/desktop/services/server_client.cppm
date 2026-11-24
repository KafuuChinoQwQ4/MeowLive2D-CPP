module;

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <utility>

export module meowlive.desktop.services.server_client;

import meowlive.protocol.control;

export namespace meowlive::desktop::services
{
    enum class AdminSession
    {
        Unavailable,
        Disabled,
        SignedOut,
        SignedIn,
    };

    class ServerClient final : public ::QObject
    {
    public:
        explicit ServerClient(::QUrl origin) : origin_(std::move(origin))
        {
            network_.setProxy(::QNetworkProxy::NoProxy);
            network_.setRedirectPolicy(::QNetworkRequest::ManualRedirectPolicy);
            network_.setTransferTimeout(5'000);
        }

        void get_health(std::function<void(const ::QString&)> complete)
        {
            ::QUrl url = origin_;
            const auto path = meowlive::protocol::health_path;
            url.setPath(::QString::fromLatin1(path.data(), static_cast<::qsizetype>(path.size())));
            ::QNetworkReply* reply = network_.get(::QNetworkRequest(url));
            this->connect(reply, &::QNetworkReply::finished, this,
                          [reply, complete = std::move(complete)]
                          {
                              reply->deleteLater();
                              if (reply->error() != ::QNetworkReply::NoError)
                              {
                                  complete(QStringLiteral("主服务不可用"));
                                  return;
                              }
                              const auto json =
                                  ::QJsonDocument::fromJson(reply->readAll()).object();
                              if (!json.value(QStringLiteral("bridge_connected")).isBool() ||
                                  json.value(QStringLiteral("service")).toString() !=
                                      QStringLiteral("meowlive") ||
                                  json.value(QStringLiteral("protocol_version")).toInt() !=
                                      meowlive::protocol::version)
                              {
                                  complete(QStringLiteral("主服务协议不匹配"));
                                  return;
                              }
                              complete(QStringLiteral("主服务在线"));
                          });
        }

        void get_admin_session(std::function<void(AdminSession)> complete)
        {
            ::QNetworkReply* reply = network_.get(this->admin_request());
            this->connect(reply, &::QNetworkReply::finished, this,
                          [this, reply, complete = std::move(complete)]
                          {
                              reply->deleteLater();
                              const auto json =
                                  ::QJsonDocument::fromJson(reply->readAll()).object();
                              const auto enabled = json.value(QStringLiteral("enabled"));
                              const auto authenticated =
                                  json.value(QStringLiteral("authenticated"));
                              if (reply->error() != ::QNetworkReply::NoError || !enabled.isBool() ||
                                  !authenticated.isBool())
                              {
                                  complete(AdminSession::Unavailable);
                                  return;
                              }
                              if (!authenticated.toBool())
                              {
                                  session_.clear();
                              }
                              complete(!enabled.toBool()        ? AdminSession::Disabled
                                       : authenticated.toBool() ? AdminSession::SignedIn
                                                                : AdminSession::SignedOut);
                          });
        }

        void login(const ::QString& admin_token, std::function<void(bool)> complete)
        {
            auto request = this->admin_request();
            request.setHeader(::QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            const ::QJsonObject body{{QStringLiteral("token"), admin_token}};
            ::QNetworkReply* reply =
                network_.post(request, ::QJsonDocument(body).toJson(::QJsonDocument::Compact));
            this->connect(reply, &::QNetworkReply::finished, this,
                          [this, reply, complete = std::move(complete)]
                          {
                              reply->deleteLater();
                              const auto json =
                                  ::QJsonDocument::fromJson(reply->readAll()).object();
                              const auto token =
                                  json.value(QStringLiteral("token")).toString().toLatin1();
                              if (reply->error() != ::QNetworkReply::NoError || token.isEmpty())
                              {
                                  complete(false);
                                  return;
                              }
                              for (const unsigned char character : token)
                              {
                                  if (character < 0x21 || character > 0x7e)
                                  {
                                      complete(false);
                                      return;
                                  }
                              }
                              session_ = token;
                              complete(true);
                          });
        }

        void logout(std::function<void(bool)> complete)
        {
            ::QNetworkReply* reply = network_.deleteResource(this->admin_request());
            this->connect(
                reply, &::QNetworkReply::finished, this,
                [this, reply, complete = std::move(complete)]
                {
                    reply->deleteLater();
                    const auto status =
                        reply->attribute(::QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (status == 204 || status == 401)
                    {
                        session_.clear();
                        complete(true);
                        return;
                    }
                    complete(false);
                });
        }

    private:
        ::QNetworkRequest admin_request() const
        {
            ::QUrl url = origin_;
            url.setPath(QStringLiteral("/api/admin/session"));
            ::QNetworkRequest request(url);
            if (!session_.isEmpty())
            {
                request.setRawHeader(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + session_);
            }
            return request;
        }

        ::QUrl origin_;
        ::QByteArray session_;
        ::QNetworkAccessManager network_;
    };
}
