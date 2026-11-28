module;

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <expected>

export module meowlive.desktop_runtime.avatar.token;

namespace
{
    bool private_path(const ::QString& path)
    {
        auto current = ::QFileInfo(path);
        while (true)
        {
            if (current.isSymLink() || current.isJunction())
            {
                return false;
            }
            const auto parent = current.absoluteDir().absolutePath();
            if (parent == current.absoluteFilePath())
            {
                return true;
            }
            current.setFile(parent);
        }
    }
}

export namespace meowlive::desktop_runtime::avatar
{
    bool valid_token(const ::QString& value)
    {
        if (value.isEmpty() || value.size() > 64)
        {
            return false;
        }
        for (const auto character : value)
        {
            if (character.unicode() < 0x21 || character.unicode() > 0x7e)
            {
                return false;
            }
        }
        return true;
    }

    ::QString token_path()
    {
        auto directory = ::qEnvironmentVariable("MEOWLIVE_DATA_DIR");
        if (directory.isEmpty())
        {
            const auto local = ::qEnvironmentVariable("LOCALAPPDATA");
            if (local.isEmpty())
            {
                return {};
            }
            directory = ::QDir(local).filePath(QStringLiteral("MeowLive2D"));
        }
        return ::QDir(directory).absoluteFilePath(QStringLiteral("local/vts-token.json"));
    }

    std::expected<::QString, ::QString> load_token(const ::QString& path)
    {
        const auto invalid = std::unexpected(QStringLiteral("VTube Studio 本地授权文件无效"));
        if (path.isEmpty() || !private_path(path))
        {
            return invalid;
        }
        const ::QFileInfo info(path);
        if (!info.exists())
        {
            return ::QString{};
        }

        constexpr auto maximum = 4096;
        ::QFile file(path);
        if (!info.isFile() || info.size() > maximum || !file.open(::QIODevice::ReadOnly) ||
            !private_path(path))
        {
            return invalid;
        }
        const auto bytes = file.read(maximum + 1);
        const auto object = ::QJsonDocument::fromJson(bytes).object();
        const auto value = object.value(QStringLiteral("authenticationToken")).toString();
        if (file.error() != ::QFileDevice::NoError || bytes.size() > maximum ||
            !meowlive::desktop_runtime::avatar::valid_token(value))
        {
            return invalid;
        }
        return value;
    }

    bool save_token(const ::QString& path, const ::QString& token)
    {
        if (path.isEmpty() || !meowlive::desktop_runtime::avatar::valid_token(token) ||
            !private_path(path) || !::QDir().mkpath(::QFileInfo(path).absolutePath()) ||
            !private_path(path))
        {
            return false;
        }
        ::QSaveFile file(path);
        const ::QJsonObject object{{QStringLiteral("authenticationToken"), token}};
        const auto bytes = ::QJsonDocument(object).toJson(::QJsonDocument::Compact);
        return file.open(::QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
               file.commit();
    }
}
