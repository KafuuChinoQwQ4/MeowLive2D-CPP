module;

#include <QByteArray>
#include <QFile>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <expected>
#include <functional>
#include <memory>
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

    struct TrainingStatus
    {
        bool enabled{};
        bool busy{};
        ::QJsonArray jobs;
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

        void get_speeches(std::function<void(const ::QJsonArray&)> complete)
        {
            ::QNetworkReply* reply =
                network_.get(this->admin_request(QStringLiteral("/api/status")));
            this->connect(reply, &::QNetworkReply::finished, this,
                          [reply, complete = std::move(complete)]
                          {
                              reply->deleteLater();
                              if (reply->error() != ::QNetworkReply::NoError)
                              {
                                  complete({});
                                  return;
                              }
                              const auto json =
                                  ::QJsonDocument::fromJson(reply->readAll()).object();
                              complete(json.value(QStringLiteral("speeches")).toArray());
                          });
        }

        void enqueue_speech(const ::QString& text, std::function<void(const ::QString&)> complete)
        {
            this->post_speech_request(QStringLiteral("/api/speech"),
                                      {{QStringLiteral("text"), text},
                                       {QStringLiteral("voice_id"), QStringLiteral("active")}},
                                      202, std::move(complete));
        }

        void preview_speech(std::function<void(const ::QString&)> complete)
        {
            this->post_speech_request(QStringLiteral("/api/speech/preview"), {}, 202,
                                      std::move(complete));
        }

        void stop_speech(std::function<void(const ::QString&)> complete)
        {
            this->post_speech_request(QStringLiteral("/api/stop"), {}, 200, std::move(complete));
        }

        void get_json(const ::QString& path, ::QObject& receiver,
                      std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            this->finish_json(network_.get(this->admin_request(path)), receiver,
                              std::move(complete));
        }

        void get_json(const ::QString& path, const ::QUrlQuery& query, ::QObject& receiver,
                      std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            this->finish_json(network_.get(this->admin_request(path, query)), receiver,
                              std::move(complete));
        }

        void get_training_status(
            ::QObject& receiver,
            std::function<
                void(std::expected<meowlive::desktop::services::TrainingStatus, ::QString>)>
                complete)
        {
            this->get_json(QStringLiteral("/api/training"), receiver,
                           [complete = std::move(complete)](const ::QJsonDocument& document,
                                                            const ::QString& error)
                           {
                               if (!error.isEmpty())
                               {
                                   complete(std::unexpected(error));
                                   return;
                               }
                               const auto status = document.object();
                               const auto enabled = status.value(QStringLiteral("enabled"));
                               const auto busy = status.value(QStringLiteral("busy"));
                               const auto jobs = status.value(QStringLiteral("jobs"));
                               if (!enabled.isBool() || !busy.isBool() || !jobs.isArray())
                               {
                                   complete(std::unexpected(QStringLiteral("训练状态响应无效")));
                                   return;
                               }
                               complete(meowlive::desktop::services::TrainingStatus{
                                   enabled.toBool(), busy.toBool(), jobs.toArray()});
                           });
        }

        void post_json(const ::QString& path, const ::QJsonObject& body, ::QObject& receiver,
                       std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            auto request = this->admin_request(path);
            request.setHeader(::QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            request.setTransferTimeout(125'000);
            auto* reply =
                network_.post(request, ::QJsonDocument(body).toJson(::QJsonDocument::Compact));
            this->finish_json(reply, receiver, std::move(complete));
        }

        void import_voice(const ::QJsonObject& metadata, const ::QString& filename,
                          ::QObject& receiver,
                          std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            this->upload_recordings(QStringLiteral("/api/voices"), metadata, {filename}, 125'000,
                                    receiver, std::move(complete));
        }

        void transcribe(const ::QJsonObject& metadata, const ::QString& filename,
                        ::QObject& receiver,
                        std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            this->upload_recordings(QStringLiteral("/api/training/transcribe"), metadata,
                                    {filename}, 305'000, receiver, std::move(complete));
        }

        void submit_training(const ::QJsonObject& metadata, const ::QStringList& filenames,
                             ::QObject& receiver,
                             std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            this->upload_recordings(QStringLiteral("/api/training/jobs"), metadata, filenames,
                                    125'000, receiver, std::move(complete));
        }

        void audition_training(const ::QString& id, const ::QString& text, ::QObject& receiver,
                               std::function<void(::QByteArray, const ::QString&)> complete)
        {
            auto request = this->admin_request(QStringLiteral("/api/training/audition"));
            request.setTransferTimeout(305'000);
            request.setHeader(::QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            const ::QJsonObject input{{QStringLiteral("version_id"), id},
                                      {QStringLiteral("text"), text}};
            auto* reply =
                network_.post(request, ::QJsonDocument(input).toJson(::QJsonDocument::Compact));
            ::QObject::connect(
                reply, &::QNetworkReply::finished, reply,
                [reply, target = ::QPointer<::QObject>(&receiver), complete = std::move(complete)]
                {
                    reply->deleteLater();
                    if (!target)
                    {
                        return;
                    }
                    const auto body = reply->readAll();
                    if (reply->error() == ::QNetworkReply::NoError && body.startsWith("RIFF"))
                    {
                        complete(body, {});
                    }
                    else
                    {
                        const auto error = ::QJsonDocument::fromJson(body)
                                               .object()
                                               .value(QStringLiteral("message"))
                                               .toString();
                        complete({}, error.isEmpty() ? QStringLiteral("无法取得试听音频") : error);
                    }
                });
        }

    private:
        void
        upload_recordings(const ::QString& path, const ::QJsonObject& metadata,
                          const ::QStringList& filenames, int timeout, ::QObject& receiver,
                          std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            auto parts = std::make_unique<::QHttpMultiPart>(::QHttpMultiPart::FormDataType);
            ::QHttpPart info;
            info.setHeader(::QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"metadata\""));
            info.setBody(::QJsonDocument(metadata).toJson(::QJsonDocument::Compact));
            parts->append(info);

            for (const auto& filename : filenames)
            {
                auto* audio = new ::QFile(filename, parts.get());
                if (!audio->open(::QIODevice::ReadOnly))
                {
                    complete({}, QStringLiteral("无法读取参考录音"));
                    return;
                }
                ::QHttpPart recording;
                recording.setHeader(
                    ::QNetworkRequest::ContentDispositionHeader,
                    QStringLiteral("form-data; name=\"audio\"; filename=\"reference.wav\""));
                recording.setHeader(::QNetworkRequest::ContentTypeHeader,
                                    QStringLiteral("audio/wav"));
                recording.setBodyDevice(audio);
                parts->append(recording);
            }
            auto request = this->admin_request(path);
            request.setTransferTimeout(timeout);
            auto* reply = network_.post(request, parts.get());
            parts.release()->setParent(reply);
            this->finish_json(reply, receiver, std::move(complete));
        }

        static void
        finish_json(::QNetworkReply* reply, ::QObject& receiver,
                    std::function<void(const ::QJsonDocument&, const ::QString&)> complete)
        {
            ::QObject::connect(
                reply, &::QNetworkReply::finished, reply,
                [reply, target = ::QPointer<::QObject>(&receiver), complete = std::move(complete)]
                {
                    reply->deleteLater();
                    if (!target)
                    {
                        return;
                    }
                    const auto document = ::QJsonDocument::fromJson(reply->readAll());
                    if (reply->error() != ::QNetworkReply::NoError)
                    {
                        const auto body = document.object();
                        const auto message = body.value(QStringLiteral("message")).toString();
                        const auto code = body.value(QStringLiteral("code")).toString();
                        complete(document, !code.isEmpty()
                                               ? QStringLiteral("%1 · %2").arg(code, message)
                                           : message.isEmpty() ? reply->errorString()
                                                               : message);
                    }
                    else if (document.isNull())
                    {
                        complete(document, QStringLiteral("服务返回了无效的 JSON"));
                    }
                    else
                    {
                        complete(document, {});
                    }
                });
        }

        void post_speech_request(const ::QString& path, const ::QJsonObject& body,
                                 int expected_status,
                                 std::function<void(const ::QString&)> complete)
        {
            auto request = this->admin_request(path);
            request.setHeader(::QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            ::QNetworkReply* reply =
                network_.post(request, ::QJsonDocument(body).toJson(::QJsonDocument::Compact));
            this->connect(
                reply, &::QNetworkReply::finished, this,
                [reply, expected_status, complete = std::move(complete)]
                {
                    reply->deleteLater();
                    const auto status =
                        reply->attribute(::QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const auto json = ::QJsonDocument::fromJson(reply->readAll()).object();
                    if (status == expected_status && reply->error() == ::QNetworkReply::NoError)
                    {
                        complete({});
                        return;
                    }
                    const auto message = json.value(QStringLiteral("message")).toString();
                    complete(message.isEmpty() ? reply->errorString() : message);
                });
        }

        ::QNetworkRequest
        admin_request(const ::QString& path = QStringLiteral("/api/admin/session"),
                      const ::QUrlQuery& query = {}) const
        {
            ::QUrl url = origin_;
            url.setPath(path);
            if (!query.isEmpty())
            {
                url.setQuery(query);
            }
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
