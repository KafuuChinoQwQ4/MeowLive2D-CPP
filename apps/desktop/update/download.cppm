module;

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <cstdint>
#include <expected>

#include <memory>

export module meowlive.desktop.update.download;

import meowlive.desktop.update.manifest;

namespace
{
    namespace update = meowlive::desktop::update;

    bool download(::QNetworkAccessManager& access, const ::QUrl& url, ::QIODevice& destination,
                  std::int64_t limit)
    {
        ::QNetworkRequest request(url);
        request.setAttribute(::QNetworkRequest::RedirectPolicyAttribute,
                             ::QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(20'000);
        request.setMaximumRedirectsAllowed(4);
        const std::unique_ptr<::QNetworkReply> reply(access.get(request));
        ::QEventLoop waiting;
        ::QTimer timeout;
        timeout.setSingleShot(true);
        ::QObject::connect(&timeout, &::QTimer::timeout, reply.get(), &::QNetworkReply::abort);
        std::int64_t received{};
        bool failed{};
        const auto drain = [&]
        {
            while (!failed && reply->bytesAvailable() > 0)
            {
                const auto bytes = reply->read(64 * 1024);
                if (bytes.isEmpty() || bytes.size() > limit - received ||
                    destination.write(bytes) != bytes.size())
                {
                    failed = true;
                    reply->abort();
                    return;
                }
                received += bytes.size();
            }
        };
        reply->setReadBufferSize(128 * 1024);
        ::QObject::connect(reply.get(), &::QNetworkReply::readyRead, &waiting, drain);
        ::QObject::connect(reply.get(), &::QNetworkReply::finished, &waiting, &::QEventLoop::quit);
        timeout.start(180'000);
        if (reply->isRunning())
        {
            waiting.exec();
        }
        drain();
        const bool success =
            !failed && reply->error() == ::QNetworkReply::NoError &&
            reply->attribute(::QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        return success;
    }

    bool matches(::QFile& file, const update::Chunk& expected)
    {
        if (!file.open(::QIODevice::ReadOnly) || file.size() != expected.size)
        {
            return false;
        }
        ::QCryptographicHash hash(::QCryptographicHash::Sha256);
        return hash.addData(&file) && hash.result() == expected.digest;
    }

    ::QUrl address(const ::QString& tag, const ::QString& name, bool chunks = false)
    {
        auto result = ::QUrl(QStringLiteral("https://github.com"));
        auto path = QStringLiteral("/KafuuChinoQwQ4/MeowLive2D-CPP/releases/download/") + tag;
        if (chunks)
        {
            path += QStringLiteral("-updates-windows-x86_64");
        }
        result.setPath(path + QStringLiteral("/") + name);
        return result;
    }

    ::QByteArray from_cache(const ::QDir& cache, const update::Chunk& chunk)
    {
        ::QFile input(cache.filePath(QString::fromLatin1(chunk.digest.toHex())));
        if (matches(input, chunk) && input.seek(0))
        {
            return input.readAll();
        }
        return {};
    }

    ::QByteArray fetch_chunk(::QNetworkAccessManager& access, const ::QDir& cache,
                             const ::QString& tag, const update::Chunk& chunk)
    {
        const auto key = QString::fromLatin1(chunk.digest.toHex());
        const auto name = QStringLiteral("chunk-") + key + QStringLiteral(".bin");
        for (const bool technical : {true, false})
        {
            ::QSaveFile file(cache.filePath(key + QStringLiteral(".fetch")));
            if (file.open(::QIODevice::WriteOnly) &&
                download(access, address(tag, name, technical), file, chunk.size) && file.commit())
            {
                ::QFile fetched(cache.filePath(key + QStringLiteral(".fetch")));
                if (matches(fetched, chunk) && fetched.seek(0))
                {
                    const auto bytes = fetched.readAll();
                    fetched.close();
                    ::QSaveFile verified(cache.filePath(key));
                    if (verified.open(::QIODevice::WriteOnly) &&
                        verified.write(bytes) == bytes.size())
                    {
                        verified.commit();
                    }
                    fetched.remove();
                    return bytes;
                }
                fetched.close();
                fetched.remove();
            }
        }
        return {};
    }

    bool assemble(::QNetworkAccessManager& access, const ::QDir& cache,
                  const update::Manifest& manifest, const ::QString& output)
    {
        ::QSaveFile target(output);
        if (!target.open(::QIODevice::WriteOnly))
        {
            return false;
        }
        ::QCryptographicHash hash(::QCryptographicHash::Sha256);
        for (const auto& chunk : manifest.chunks)
        {
            auto data = from_cache(cache, chunk);
            if (data.isEmpty())
            {
                data = fetch_chunk(access, cache, manifest.tag, chunk);
            }
            if (data.size() != chunk.size || target.write(data) != data.size())
            {
                return false;
            }
            hash.addData(data);
        }
        return hash.result() == manifest.file.digest && target.commit();
    }
}

export namespace meowlive::desktop::update
{
    std::expected<meowlive::desktop::update::Manifest, ::QString>
    read_cached_manifest(const ::QString& tag, const ::QString& current, const ::QByteArray& key,
                         const ::QDir& storage)
    {
        ::QFile envelope(storage.filePath(QStringLiteral("manifest.json")));
        constexpr auto limit = 2 * 1024 * 1024;
        if (!envelope.open(::QIODevice::ReadOnly) || envelope.size() > limit)
        {
            return std::unexpected(QStringLiteral("无法读取签名更新清单"));
        }
        const auto bytes = envelope.read(limit + 1);
        if (bytes.size() > limit || envelope.error() != ::QFileDevice::NoError)
        {
            return std::unexpected(QStringLiteral("无法读取签名更新清单"));
        }
        return meowlive::desktop::update::verify_manifest(bytes, key, tag, current);
    }

    bool installer_matches(const ::QString& path, const meowlive::desktop::update::Chunk& expected)
    {
        ::QFile installer(path);
        return !::QFileInfo(path).isSymLink() && matches(installer, expected);
    }

    std::expected<::QString, ::QString> prepare_update(const ::QString& tag,
                                                       const ::QString& current,
                                                       const ::QByteArray& key,
                                                       const ::QDir& storage)
    {
        if (!meowlive::desktop::update::valid_tag(tag) || key.size() != 32 ||
            !storage.mkpath(QStringLiteral("chunks")))
        {
            return std::unexpected(QStringLiteral("更新版本、公钥或缓存目录未配置"));
        }
        ::QNetworkAccessManager access;
        const auto envelope_path = storage.filePath(QStringLiteral("manifest.json"));
        {
            ::QSaveFile envelope(envelope_path);
            if (!envelope.open(::QIODevice::WriteOnly) ||
                !download(access, address(tag, QStringLiteral("meowlive-update.json")), envelope,
                          2 * 1024 * 1024) ||
                !envelope.commit())
            {
                return std::unexpected(QStringLiteral("无法获取签名更新清单"));
            }
        }
        ::QFile envelope(envelope_path);
        if (!envelope.open(::QIODevice::ReadOnly) || envelope.size() > 2 * 1024 * 1024)
        {
            return std::unexpected(QStringLiteral("无法读取签名更新清单"));
        }
        const auto manifest =
            meowlive::desktop::update::verify_manifest(envelope.readAll(), key, tag, current);
        if (!manifest)
        {
            return std::unexpected(manifest.error());
        }
        const auto path = storage.filePath(manifest->installer);
        {
            ::QFile existing(path);
            if (matches(existing, manifest->file))
            {
                return path;
            }
        }
        const ::QDir cache(storage.filePath(QStringLiteral("chunks")));
        if (assemble(access, cache, *manifest, path))
        {
            return path;
        }
        const auto full = storage.filePath(QStringLiteral("installer.download"));
        {
            ::QSaveFile target(full);
            if (!target.open(::QIODevice::WriteOnly) ||
                !download(access, address(tag, manifest->installer), target, manifest->file.size) ||
                !target.commit())
            {
                return std::unexpected(QStringLiteral("无法下载完整安装包"));
            }
        }
        ::QFile installer(full);
        if (!matches(installer, manifest->file))
        {
            installer.close();
            installer.remove();
            return std::unexpected(QStringLiteral("安装包校验失败"));
        }
        installer.close();
        if ((::QFileInfo::exists(path) && !::QFile::remove(path)) || !installer.rename(path))
        {
            return std::unexpected(QStringLiteral("无法保存已校验的安装包"));
        }
        return path;
    }
}
