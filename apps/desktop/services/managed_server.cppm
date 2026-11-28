module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>
#include <utility>

export module meowlive.desktop.services.managed_server;

import meowlive.protocol.control;
import meowlive.protocol.managed_server;

export namespace meowlive::desktop::services
{
    enum class UpdateStopResult
    {
        Stopped,
        Busy,
        Failed,
    };

    // The caller owns the process and keeps its input channel open until the server replies.
    meowlive::desktop::services::UpdateStopResult request_update_stop(::QProcess& process)
    {
        namespace protocol = meowlive::protocol::managed_server;
        using Result = meowlive::desktop::services::UpdateStopResult;
        if (process.state() != ::QProcess::Running)
        {
            return Result::Failed;
        }

        process.readAllStandardOutput();
        const auto deadline = ::GetTickCount64() + protocol::admission_window_ms;
        const auto request = ::QByteArray::number(static_cast<::qulonglong>(deadline));
        const auto command = ::QByteArray(protocol::stop_command) + request + '\n';
        if (process.write(command) != command.size())
        {
            return Result::Failed;
        }

        ::QElapsedTimer timer;
        timer.start();
        const auto busy = ::QByteArray(protocol::busy_reply) + ' ' + request;
        const auto stopping = ::QByteArray(protocol::stopping_reply) + ' ' + request;
        ::QByteArray pending;
        const auto read_reply = [&]() -> std::optional<Result>
        {
            pending += process.readAllStandardOutput();
            ::qsizetype end{};
            while ((end = pending.indexOf('\n')) >= 0)
            {
                auto line = pending.first(end);
                pending.remove(0, end + 1);
                if (line.endsWith('\r'))
                {
                    line.chop(1);
                }
                if (line == busy)
                {
                    return Result::Busy;
                }
                if (line == stopping)
                {
                    const bool exited = process.state() == ::QProcess::NotRunning ||
                                        process.waitForFinished(protocol::shutdown_timeout_ms);
                    return exited && process.exitStatus() == ::QProcess::NormalExit &&
                                   process.exitCode() == 0
                               ? Result::Stopped
                               : Result::Failed;
                }
            }
            if (pending.size() > 4'096)
            {
                pending.clear();
            }
            return std::nullopt;
        };

        while (true)
        {
            if (const auto reply = read_reply())
            {
                return *reply;
            }
            const auto remaining = protocol::reply_timeout_ms - timer.elapsed();
            if (remaining <= 0 || process.state() == ::QProcess::NotRunning ||
                process.error() == ::QProcess::WriteError ||
                process.error() == ::QProcess::ReadError)
            {
                return Result::Failed;
            }
            // QProcess also flushes queued stdin writes while waiting for stdout.
            process.waitForReadyRead(static_cast<int>(remaining));
        }
    }

    class ManagedServer final : public ::QObject
    {
    public:
        using Status = std::function<void(const ::QString&, bool retryable)>;

        ManagedServer(::QUrl origin, Status status)
            : origin_(std::move(origin)), status_(std::move(status))
        {
            network_.setProxy(::QNetworkProxy::NoProxy);
            network_.setTransferTimeout(1'200);
            network_.setRedirectPolicy(::QNetworkRequest::ManualRedirectPolicy);
            timer_.setSingleShot(true);
            this->connect(&timer_, &::QTimer::timeout, this, &ManagedServer::probe);
            this->connect(
                &process_, &::QProcess::errorOccurred, this,
                [this](::QProcess::ProcessError error)
                {
                    if (error == ::QProcess::FailedToStart && !stopping_)
                    {
                        fail(QStringLiteral("无法启动主服务：%1").arg(process_.errorString()));
                    }
                });
            this->connect(
                &process_, &::QProcess::finished, this,
                [this](int, ::QProcess::ExitStatus)
                {
                    if (!stopping_ && !retryable_)
                    {
                        fail(QStringLiteral("受管主服务已退出，请检查配置、端口和运行日志"));
                    }
                });
        }

        ~ManagedServer() override
        {
            stopping_ = true;
            timer_.stop();
            stop_process();
        }

        void start()
        {
            if (pending_ || process_.state() != ::QProcess::NotRunning)
            {
                return;
            }
            retryable_ = false;
            launched_ = false;
            status_(QStringLiteral("正在检查本机主服务"), false);
            probe();
        }

        meowlive::desktop::services::UpdateStopResult stop_for_update()
        {
            using Result = meowlive::desktop::services::UpdateStopResult;
            if (pending_ || !launched_ || process_.state() != ::QProcess::Running)
            {
                return Result::Failed;
            }
            stopping_ = true;
            timer_.stop();
            const auto result = meowlive::desktop::services::request_update_stop(process_);
            stopping_ = false;
            if (result == Result::Stopped)
            {
                launched_ = false;
                status_(QStringLiteral("受管主服务已停止"), true);
            }
            else if (process_.state() == ::QProcess::NotRunning)
            {
                launched_ = false;
                fail(QStringLiteral("受管主服务未确认正常停止，请检查运行日志"));
            }
            return result;
        }

    private:
        void probe()
        {
            ::QUrl url(origin_);
            url.setPath(QStringLiteral("/api/health"));
            pending_ = true;
            auto* reply = network_.get(::QNetworkRequest(url));
            this->connect(reply, &::QNetworkReply::finished, this,
                          [this, reply]
                          {
                              reply->deleteLater();
                              pending_ = false;
                              if (stopping_ || retryable_)
                              {
                                  return;
                              }
                              const auto status =
                                  reply->attribute(::QNetworkRequest::HttpStatusCodeAttribute);
                              const auto data =
                                  ::QJsonDocument::fromJson(reply->readAll()).object();
                              const bool healthy =
                                  status.toInt() == 200 &&
                                  data.value(QStringLiteral("service")).toString() ==
                                      QStringLiteral("meowlive") &&
                                  data.value(QStringLiteral("protocol_version")).toInt() ==
                                      meowlive::protocol::version;
                              if (healthy)
                              {
                                  status_(launched_ ? QStringLiteral("受管主服务已启动")
                                                    : QStringLiteral("已连接本机主服务"),
                                          false);
                              }
                              else if (status.isValid())
                              {
                                  fail(QStringLiteral("主服务端口已被占用或协议不匹配"));
                                  stop_process();
                              }
                              else if (!launched_)
                              {
                                  launch();
                              }
                              else if (startup_.elapsed() >= 10'000)
                              {
                                  fail(QStringLiteral("主服务启动超时，请检查运行日志后重试"));
                                  stop_process();
                              }
                              else
                              {
                                  timer_.start(250);
                              }
                          });
        }

        void launch()
        {
            const ::QDir application_dir(::QCoreApplication::applicationDirPath());
            const auto executable = application_dir.filePath(QStringLiteral("meowlive-server.exe"));
            if (!::QFileInfo(executable).isFile())
            {
                fail(QStringLiteral("安装目录缺少 meowlive-server.exe"));
                return;
            }

            auto data = ::qEnvironmentVariable("MEOWLIVE_DATA_DIR");
            if (data.isEmpty())
            {
                const auto local = ::qEnvironmentVariable("LOCALAPPDATA");
                if (local.isEmpty())
                {
                    fail(QStringLiteral("无法确定 Windows 用户数据目录"));
                    return;
                }
                data = ::QDir(local).filePath(QStringLiteral("MeowLive2D"));
            }
            if (!::QDir{}.mkpath(data))
            {
                fail(QStringLiteral("无法创建本机数据目录"));
                return;
            }

            launched_ = true;
            startup_.start();
            status_(QStringLiteral("正在启动 Windows 主服务"), false);
            process_.setWorkingDirectory(application_dir.absolutePath());
            const ::QDir data_directory(data);
            const auto startup_log = data_directory.mkpath(QStringLiteral("logs"))
                                         ? QStringLiteral("logs/meowlive-server.startup.log")
                                         : QStringLiteral("meowlive-server.startup.log");
            process_.setStandardErrorFile(data_directory.absoluteFilePath(startup_log));
            process_.start(executable, {QStringLiteral("--managed"), QStringLiteral("--data-dir"),
                                        data_directory.absolutePath()});
            timer_.start(250);
        }

        void stop_process()
        {
            if (process_.state() != ::QProcess::NotRunning)
            {
                process_.closeWriteChannel();
                if (!process_.waitForFinished(23'000))
                {
                    process_.kill();
                    process_.waitForFinished(2'000);
                }
            }
        }

        void fail(const ::QString& message)
        {
            timer_.stop();
            retryable_ = true;
            status_(message, true);
        }

        const ::QUrl origin_;
        const Status status_;
        ::QNetworkAccessManager network_;
        ::QProcess process_;
        ::QTimer timer_;
        ::QElapsedTimer startup_;
        bool launched_{};
        bool pending_{};
        bool retryable_{};
        bool stopping_{};
    };
}
