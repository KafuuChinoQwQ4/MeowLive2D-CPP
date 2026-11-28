module;

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>

#include <meowlive/msvc_stl.hpp>

export module meowlive.desktop_runtime.obs.settings;

import meowlive.standard;

export namespace meowlive::desktop_runtime::obs
{
    struct Settings
    {
        bool enabled{};
        ::QString websocket_url{QStringLiteral("ws://127.0.0.1:4455")};
        ::QString password;
    };

    struct SettingsView
    {
        bool enabled{};
        ::QString websocket_url;
        bool password_configured{};
    };

    bool valid_endpoint(const ::QString& address)
    {
        if (!address.startsWith(QStringLiteral("ws://")) || address.contains(u'@') ||
            address.contains(u'?') || address.contains(u'#') || address.contains(u'\\') ||
            address.contains(u'%'))
        {
            return false;
        }
        for (const auto character : address)
        {
            if (character.isSpace() || character.category() == ::QChar::Other_Control)
            {
                return false;
            }
        }
        const ::QUrl url(address, ::QUrl::StrictMode);
        if (!url.isValid() || url.port(-1) == 0 ||
            (!url.path().isEmpty() && url.path() != QStringLiteral("/")))
        {
            return false;
        }
        auto raw = address.mid(5);
        if (raw.endsWith(u'/'))
        {
            raw.chop(1);
        }
        if (raw.contains(u'/'))
        {
            return false;
        }
        if (raw.startsWith(u'['))
        {
            const auto end = raw.indexOf(u']');
            return end > 1 && ::QHostAddress(raw.mid(1, end - 1)) == ::QHostAddress::LocalHostIPv6;
        }
        const auto host = raw.section(u':', 0, 0);
        const ::QHostAddress ip(host);
        return ip.protocol() == ::QAbstractSocket::IPv4Protocol && ip.isLoopback() &&
               ip.toString() == host;
    }

    bool valid_password(const ::QString& password)
    {
        if (password.toUtf8().size() > 4'096)
        {
            return false;
        }
        for (const auto character : password)
        {
            if (character.category() == ::QChar::Other_Control)
            {
                return false;
            }
        }
        return true;
    }

    std::expected<meowlive::desktop_runtime::obs::Settings, ::QString>
    load_settings(const ::QString& path)
    {
        meowlive::desktop_runtime::obs::Settings settings;
        const ::QFileInfo info(path);
        if (info.isSymLink())
        {
            return std::unexpected(QStringLiteral("OBS 设置文件路径无效"));
        }
        if (path.isEmpty() || !info.exists())
        {
            settings.password = ::qEnvironmentVariable("MEOWLIVE_OBS_PASSWORD");
            if (!meowlive::desktop_runtime::obs::valid_password(settings.password))
            {
                return std::unexpected(QStringLiteral("OBS 环境变量密码格式无效"));
            }
            return settings;
        }
        ::QFile file(path);
        constexpr auto limit = 32'768;
        if (!info.isFile() || info.size() > limit || !file.open(::QIODevice::ReadOnly))
        {
            return std::unexpected(QStringLiteral("无法读取 OBS 设置文件"));
        }
        const auto bytes = file.read(limit + 1);
        const auto data = ::QJsonDocument::fromJson(bytes).object();
        const auto enabled = data.value(QStringLiteral("enabled"));
        const auto url = data.value(QStringLiteral("websocket_url"));
        const auto password = data.value(QStringLiteral("password"));
        if (file.error() != ::QFileDevice::NoError || bytes.size() > limit || !enabled.isBool() ||
            !meowlive::desktop_runtime::obs::valid_endpoint(url.toString()) ||
            (!password.isNull() && !password.isString()) ||
            !meowlive::desktop_runtime::obs::valid_password(password.toString()))
        {
            return std::unexpected(QStringLiteral("OBS 设置文件格式无效"));
        }
        return meowlive::desktop_runtime::obs::Settings{enabled.toBool(), url.toString(),
                                                        password.toString()};
    }

    meowlive::desktop_runtime::obs::SettingsView
    settings_view(const meowlive::desktop_runtime::obs::Settings& value)
    {
        return {value.enabled, value.websocket_url, !value.password.isEmpty()};
    }

    std::expected<meowlive::desktop_runtime::obs::SettingsView, ::QString>
    save_settings(const ::QString& path, bool enabled, const ::QString& endpoint,
                  const ::QString& password, bool clear_password)
    {
        if (!meowlive::desktop_runtime::obs::valid_endpoint(endpoint))
        {
            return std::unexpected(QStringLiteral("OBS 地址须为本机 IP 的 ws:// 地址"));
        }
        if (!meowlive::desktop_runtime::obs::valid_password(password) ||
            (clear_password && !password.isEmpty()))
        {
            return std::unexpected(QStringLiteral("请输入有效密码，或单独选择清除已保存密码"));
        }
        auto current = meowlive::desktop_runtime::obs::load_settings(path);
        if (endpoint != (current ? current->websocket_url : ::QString{}) && password.isEmpty() &&
            !clear_password)
        {
            return std::unexpected(QStringLiteral("更改 OBS 地址后，请重新输入密码或明确清除密码"));
        }
        const auto stored_password =
            !clear_password && password.isEmpty() && current ? current->password : password;
        const ::QJsonObject data{{QStringLiteral("enabled"), enabled},
                                 {QStringLiteral("websocket_url"), endpoint},
                                 {QStringLiteral("password"), stored_password.isEmpty()
                                                                  ? ::QJsonValue::Null
                                                                  : ::QJsonValue(stored_password)}};
        const auto bytes = ::QJsonDocument(data).toJson(::QJsonDocument::Compact);
        const ::QFileInfo info(path);
        ::QSaveFile file(path);
        if (path.isEmpty() || info.isSymLink() || !::QDir().mkpath(info.absolutePath()) ||
            !file.open(::QIODevice::WriteOnly) ||
            !file.setPermissions(::QFileDevice::ReadOwner | ::QFileDevice::WriteOwner) ||
            file.write(bytes) != bytes.size() || !file.commit())
        {
            return std::unexpected(QStringLiteral("无法保存本机 OBS 设置"));
        }
        return meowlive::desktop_runtime::obs::SettingsView{enabled, endpoint,
                                                            !stored_password.isEmpty()};
    }
}
