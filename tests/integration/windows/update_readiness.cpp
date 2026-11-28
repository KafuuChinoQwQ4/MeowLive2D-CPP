#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <array>
#include <cstdio>
#include <expected>
#include <optional>
#include <stdexcept>
#include <utility>

import meowlive.desktop.services.managed_server;
import meowlive.desktop.services.server_client;
import meowlive.desktop.services.update_readiness;
import meowlive.protocol.managed_server;

namespace
{
    using DocumentResult = std::pair<::QJsonDocument, ::QString>;
    using TrainingResult = std::expected<meowlive::desktop::services::TrainingStatus, ::QString>;

    void require(bool condition, const ::QString& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message.toStdString());
        }
    }

    template <typename Result, typename Request> Result await(Request request)
    {
        std::optional<Result> result;
        ::QEventLoop loop;
        ::QTimer::singleShot(8'000, &loop, &::QEventLoop::quit);
        request(loop,
                [&](Result value)
                {
                    result.emplace(std::move(value));
                    loop.quit();
                });
        if (!result)
        {
            loop.exec();
        }
        require(result.has_value(), QStringLiteral("Desktop update request timed out"));
        return std::move(*result);
    }

    void write_file(const ::QString& path, const ::QByteArray& bytes)
    {
        ::QFile file(path);
        require(file.open(::QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                QStringLiteral("Cannot write update fixture: %1").arg(path));
    }

    ::QByteArray recording()
    {
        constexpr ::quint32 sample_rate = 8'000;
        constexpr ::quint32 bytes = sample_rate * 3 * 2;
        ::QByteArray wav;
        ::QDataStream writer(&wav, ::QIODevice::WriteOnly);
        writer.setByteOrder(::QDataStream::LittleEndian);
        writer.writeRawData("RIFF", 4);
        writer << ::quint32{36 + bytes};
        writer.writeRawData("WAVEfmt ", 8);
        writer << ::quint32{16} << ::quint16{1} << ::quint16{1} << sample_rate
               << ::quint32{sample_rate * 2} << ::quint16{2} << ::quint16{16};
        writer.writeRawData("data", 4);
        writer << bytes;
        ::QByteArray samples(bytes, '\0');
        samples[0] = '\1';
        writer.writeRawData(samples.constData(), samples.size());
        return wav;
    }

    class Fixture
    {
    public:
        explicit Fixture(::QString executable) : executable_(std::move(executable))
        {
            ::QTcpServer listening;
            require(listening.listen(::QHostAddress::LocalHost),
                    QStringLiteral("Cannot reserve an isolated server port"));
            const auto port = listening.serverPort();
            listening.close();
            origin_ = ::QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));

            require(data_.isValid(), QStringLiteral("Cannot create an isolated data directory"));
            const ::QDir storage(data_.path());
            const auto root = storage.absoluteFilePath(QStringLiteral("engine"));
            const auto runner = storage.absoluteFilePath(QStringLiteral("fixture-trainer.py"));
            require(storage.mkpath(QStringLiteral("engine/GPT_SoVITS")),
                    QStringLiteral("Cannot create the fixture training engine directory"));
            write_file(root + QStringLiteral("/GPT_SoVITS/s1_train.py"), {});
            write_file(root + QStringLiteral("/GPT_SoVITS/s2_train.py"), {});
            write_file(runner, {});
            write_file(this->audio_path(), recording());
            write_file(storage.absoluteFilePath(QStringLiteral("server.toml")),
                       QStringLiteral("[server]\nlisten_address = \"127.0.0.1:%1\"\n"
                                      "[auth]\nenabled = false\n"
                                      "[speech]\nreference_audio = \"clip.wav\"\n")
                           .arg(port)
                           .toUtf8());

            auto environment = ::QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("MEOWLIVE_TRAINING_PYTHON"),
                               ::QCoreApplication::applicationFilePath());
            environment.insert(QStringLiteral("MEOWLIVE_TRAINING_RUNNER"), runner);
            environment.insert(QStringLiteral("MEOWLIVE_GPT_SOVITS_ROOT"), root);
            server_.setProcessEnvironment(environment);
            this->start();
        }

        ~Fixture()
        {
            if (server_.state() != ::QProcess::NotRunning)
            {
                server_.closeWriteChannel();
                if (!server_.waitForFinished(5'000))
                {
                    server_.kill();
                    server_.waitForFinished(2'000);
                }
            }
        }

        ::QString audio_path() const
        {
            return data_.filePath(QStringLiteral("clip.wav"));
        }

        const ::QUrl& origin() const
        {
            return origin_;
        }

        ::QDir job_directory(const ::QString& id) const
        {
            return ::QDir(data_.filePath(QStringLiteral("training/jobs/") + id));
        }

        void restart()
        {
            require(this->request_update_stop() ==
                        meowlive::desktop::services::UpdateStopResult::Stopped,
                    QStringLiteral("Idle fixture refused the conditional update stop"));
            this->start();
        }

        meowlive::desktop::services::UpdateStopResult request_update_stop()
        {
            return meowlive::desktop::services::request_update_stop(server_);
        }

        void wait_for_health(meowlive::desktop::services::ServerClient& client)
        {
            ::QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 8'000 && server_.state() != ::QProcess::NotRunning)
            {
                const auto [health, error] = await<DocumentResult>(
                    [&](::QObject& receiver, auto complete)
                    {
                        client.get_json(
                            QStringLiteral("/api/health"), receiver,
                            [complete](const ::QJsonDocument& document, const ::QString& problem)
                            {
                                complete({document, problem});
                            });
                    });
                if (error.isEmpty() &&
                    health.object().value(QStringLiteral("service")) == QStringLiteral("meowlive"))
                {
                    return;
                }
                ::QThread::msleep(20);
            }
            throw std::runtime_error("Fixture server was not ready: " +
                                     server_.readAllStandardError().toStdString());
        }

    private:
        void start()
        {
            server_.start(executable_, {QStringLiteral("--managed"), QStringLiteral("--data-dir"),
                                        data_.path()});
            require(server_.waitForStarted(), server_.errorString());
        }

        ::QTemporaryDir data_;
        ::QProcess server_;
        ::QUrl origin_;
        ::QString executable_;
    };

    TrainingResult training_status(meowlive::desktop::services::ServerClient& client)
    {
        return await<TrainingResult>(
            [&](::QObject& receiver, auto complete)
            {
                client.get_training_status(receiver, complete);
            });
    }

    ::QString update_readiness(meowlive::desktop::services::ServerClient& client)
    {
        return await<::QString>(
            [&](::QObject& receiver, auto complete)
            {
                meowlive::desktop::services::check_update_readiness(client, receiver, complete);
            });
    }

    ::QJsonObject find_job(const meowlive::desktop::services::TrainingStatus& training,
                           const ::QString& id)
    {
        for (const auto& entry : training.jobs)
        {
            if (const auto job = entry.toObject(); job.value(QStringLiteral("id")) == id)
            {
                return job;
            }
        }
        return {};
    }

    template <typename Predicate>
    void wait_for_training(meowlive::desktop::services::ServerClient& client, const ::QString& id,
                           Predicate matches, const ::QString& message)
    {
        ::QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 8'000)
        {
            const auto training = training_status(client);
            if (training)
            {
                const auto job = find_job(*training, id);
                if (!job.isEmpty() && matches(*training, job))
                {
                    return;
                }
            }
            ::QThread::msleep(20);
        }
        throw std::runtime_error(message.toStdString());
    }

    void check_training_persistence(Fixture& fixture,
                                    meowlive::desktop::services::ServerClient& client,
                                    const ::QString& id)
    {
        wait_for_training(
            client, id,
            [](const auto& training, const ::QJsonObject& job)
            {
                return training.busy &&
                       job.value(QStringLiteral("status")) == QStringLiteral("training");
            },
            QStringLiteral("Fixture trainer did not report progress"));
        const auto directory = fixture.job_directory(id);
        const auto status = directory.filePath(QStringLiteral("status.json"));
        require(
            ::QFile::rename(status, directory.filePath(QStringLiteral("status.original.json"))) &&
                directory.mkdir(QStringLiteral("status.json")) &&
                directory.mkdir(QStringLiteral("outcome.json")),
            QStringLiteral("Cannot obstruct both terminal status paths"));
        const auto [cancelled, error] = await<DocumentResult>(
            [&](::QObject& receiver, auto complete)
            {
                client.post_json(
                    QStringLiteral("/api/training/cancel"), {{QStringLiteral("id"), id}}, receiver,
                    [complete](const ::QJsonDocument& document, const ::QString& problem)
                    {
                        complete({document, problem});
                    });
            });
        require(error.isEmpty() && cancelled.isObject(),
                QStringLiteral("Fixture training cancellation failed: %1").arg(error));
        wait_for_training(
            client, id,
            [](const auto& training, const ::QJsonObject& job)
            {
                return training.busy && job.value(QStringLiteral("save_pending")).toBool();
            },
            QStringLiteral("Training released busy status before durable persistence"));
        require(update_readiness(client) == QStringLiteral("请等待语音训练结束"),
                QStringLiteral("Unsaved terminal result did not block updates"));
        require(fixture.request_update_stop() ==
                    meowlive::desktop::services::UpdateStopResult::Busy,
                QStringLiteral("Unsaved terminal result did not block the conditional stop"));

        require(directory.rmdir(QStringLiteral("outcome.json")),
                QStringLiteral("Cannot restore the alternate terminal status path"));
        const auto stored_cancelled = [](const auto& training, const ::QJsonObject& job)
        {
            return !training.busy && !job.value(QStringLiteral("save_pending")).toBool() &&
                   job.value(QStringLiteral("status")) == QStringLiteral("cancelled");
        };
        wait_for_training(client, id, stored_cancelled,
                          QStringLiteral("Training did not recover terminal status after retry"));
        require(::QFileInfo(status).isDir() &&
                    ::QFileInfo(directory.filePath(QStringLiteral("outcome.json"))).isFile(),
                QStringLiteral("Terminal status did not use the available fallback file"));
        require(update_readiness(client).isEmpty(),
                QStringLiteral("Durable terminal status did not release update readiness"));

        fixture.restart();
        meowlive::desktop::services::ServerClient restarted(fixture.origin());
        fixture.wait_for_health(restarted);
        wait_for_training(restarted, id, stored_cancelled,
                          QStringLiteral("Restart did not recover the stored cancellation"));
    }

    void check_training_gate(const ::QString& executable)
    {
        Fixture fixture(executable);
        meowlive::desktop::services::ServerClient client(fixture.origin());
        fixture.wait_for_health(client);
        const auto idle = training_status(client);
        require(idle && idle->enabled && !idle->busy,
                idle ? QStringLiteral("Expected the idle training fixture") : idle.error());
        const auto ready = update_readiness(client);
        require(ready.isEmpty(), QStringLiteral("Idle server rejected updates: %1").arg(ready));

        const ::QJsonObject clip{{QStringLiteral("language"), QStringLiteral("en")},
                                 {QStringLiteral("text"), QStringLiteral("Fixture recording")}};
        const ::QJsonObject metadata{{QStringLiteral("name"), QStringLiteral("Update fixture")},
                                     {QStringLiteral("voice_id"), QStringLiteral("default")},
                                     {QStringLiteral("model_version"), QStringLiteral("v2")},
                                     {QStringLiteral("gpt_epochs"), 1},
                                     {QStringLiteral("sovits_epochs"), 1},
                                     {QStringLiteral("reviewed"), true},
                                     {QStringLiteral("clips"), ::QJsonArray{clip, clip}}};
        const auto [job, submitted] = await<DocumentResult>(
            [&](::QObject& receiver, auto complete)
            {
                const auto wav = fixture.audio_path();
                client.submit_training(
                    metadata, {wav, wav}, receiver,
                    [complete](const ::QJsonDocument& document, const ::QString& error)
                    {
                        complete({document, error});
                    });
            });
        require(submitted.isEmpty() && job.object().value(QStringLiteral("id")).isString(),
                QStringLiteral("Fixture training submission failed: %1").arg(submitted));
        const auto running = training_status(client);
        require(running && running->busy, QStringLiteral("Fixture training was not active"));
        const auto blocked = update_readiness(client);
        require(blocked == QStringLiteral("请等待语音训练结束"),
                QStringLiteral("Active training did not block updates: %1").arg(blocked));
        require(fixture.request_update_stop() ==
                    meowlive::desktop::services::UpdateStopResult::Busy,
                QStringLiteral("Active training did not block the conditional stop"));
        check_training_persistence(fixture, client,
                                   job.object().value(QStringLiteral("id")).toString());
    }

    void check_failed_stop()
    {
        using Result = meowlive::desktop::services::UpdateStopResult;
        for (const auto& behavior : {QStringLiteral("unrelated-exit"),
                                     QStringLiteral("nonzero-exit"), QStringLiteral("no-reply")})
        {
            ::QProcess child;
            child.start(::QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--stop-fixture"), behavior});
            require(child.waitForStarted(), child.errorString());
            require(meowlive::desktop::services::request_update_stop(child) == Result::Failed,
                    QStringLiteral("Unexpected process result accepted: %1").arg(behavior));
            if (behavior == QStringLiteral("no-reply"))
            {
                require(child.state() == ::QProcess::Running,
                        QStringLiteral("Unresponsive update command terminated the server"));
                child.closeWriteChannel();
                require(child.waitForFinished(5'000),
                        QStringLiteral("Fixture did not exit after ordinary pipe closure"));
            }
        }
    }

    int stop_fixture(const ::QString& behavior)
    {
        std::array<char, 64> request{};
        if (!std::fgets(request.data(), static_cast<int>(request.size()), stdin))
        {
            return 2;
        }
        if (behavior == QStringLiteral("nonzero-exit"))
        {
            namespace protocol = meowlive::protocol::managed_server;
            const auto deadline = request.data() + sizeof(protocol::stop_command) - 1;
            std::fprintf(stdout, "%s %s", protocol::stopping_reply, deadline);
            std::fflush(stdout);
            return 1;
        }
        if (behavior == QStringLiteral("no-reply"))
        {
            while (std::fgets(request.data(), static_cast<int>(request.size()), stdin))
            {
            }
        }
        return 0;
    }
}

int main(int argc, char* argv[])
{
    const ::QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.value(1) == QStringLiteral("--stop-fixture"))
    {
        return stop_fixture(arguments.value(2));
    }
    if (arguments.value(6).endsWith(QStringLiteral("fixture-trainer.py")) &&
        arguments.value(7) == QStringLiteral("--job"))
    {
        std::puts(R"({"state":"training","progress":25})");
        std::fflush(stdout);
        ::QThread::sleep(30);
        return 1;
    }
    try
    {
        const auto executable = arguments.value(1, ::QCoreApplication::applicationDirPath() +
                                                       QStringLiteral("/meowlive-server.exe"));
        check_failed_stop();
        check_training_gate(executable);
        std::puts("Desktop update readiness and durable training cancellation passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
