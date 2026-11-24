#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QMessageBox>
#include <QString>
#include <QUrl>

#include <optional>

import meowlive.desktop.services.server_client;
import meowlive.desktop.ui.main_window;
import meowlive.desktop_runtime.connection;

namespace
{
    std::optional<::QByteArray> read_device_token()
    {
        ::QByteArray token;
        if (::qEnvironmentVariableIsSet("MEOWLIVE_DEVICE_TOKEN"))
        {
            token = ::qgetenv("MEOWLIVE_DEVICE_TOKEN");
        }
        else
        {
            const auto filename = ::qEnvironmentVariable("MEOWLIVE_DEVICE_TOKEN_FILE");
            if (filename.isEmpty())
            {
                return token;
            }
            ::QFile file(filename);
            if (!file.open(::QIODevice::ReadOnly))
            {
                return std::nullopt;
            }
            token = file.read(1'025);
            if (token.size() > 1'024 || file.error() != ::QFileDevice::NoError)
            {
                return std::nullopt;
            }
        }

        token = token.trimmed();
        if (token.size() < 32 || token.size() > 512)
        {
            return std::nullopt;
        }
        for (const unsigned char character : token)
        {
            if (character < 0x21 || character > 0x7e)
            {
                return std::nullopt;
            }
        }
        return token;
    }
}

int main(int argc, char* argv[])
{
    ::QApplication app(argc, argv);
    ::QUrl origin(
        ::qEnvironmentVariable("MEOWLIVE_SERVER_URL", QStringLiteral("http://127.0.0.1:19601")),
        ::QUrl::StrictMode);
    if (origin.scheme() == QStringLiteral("ws"))
    {
        origin.setScheme(QStringLiteral("http"));
    }
    if (!origin.isValid() || origin.scheme() != QStringLiteral("http") || origin.host().isEmpty() ||
        !origin.userInfo().isEmpty() || origin.hasQuery() || origin.hasFragment() ||
        (!origin.path().isEmpty() && origin.path() != QStringLiteral("/")))
    {
        ::QMessageBox::critical(nullptr, QStringLiteral("MeowLive2D"),
                                QStringLiteral("MEOWLIVE_SERVER_URL 必须是 HTTP 服务地址"));
        return 1;
    }

    const auto token = read_device_token();
    meowlive::desktop::services::ServerClient server(origin);
    meowlive::desktop::ui::MainWindow window(server);
    meowlive::desktop_runtime::ServerConnection connection(origin, token.value_or(::QByteArray{}),
                                                           [&window](const ::QString& status)
                                                           {
                                                               window.set_bridge(status);
                                                           });

    window.show();
    if (token)
    {
        connection.start();
    }
    else
    {
        window.set_bridge(QStringLiteral("执行端凭据无效"));
    }
    return app.exec();
}
