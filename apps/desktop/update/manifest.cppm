module;

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QVersionNumber>
#include <sodium.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

export module meowlive.desktop.update.manifest;

export namespace meowlive::desktop::update
{
    struct Chunk
    {
        ::QByteArray digest;
        std::int64_t size;
    };

    struct Manifest
    {
        ::QString tag;
        ::QString installer;
        meowlive::desktop::update::Chunk file;
        std::vector<meowlive::desktop::update::Chunk> chunks;
    };
}

namespace
{
    struct Version
    {
        ::QVersionNumber number;
        std::optional<int> preview;
    };

    std::optional<Version> parse_version(const ::QString& value)
    {
        static const ::QRegularExpression format(
            QStringLiteral(R"(\Av?([0-9]+\.[0-9]+\.[0-9]+)(?:-windows-preview\.([0-9]{8}))?\z)"));
        const auto match = format.match(value);
        if (!match.hasMatch())
        {
            return std::nullopt;
        }
        const auto version = ::QVersionNumber::fromString(match.captured(1));
        if (version.segmentCount() != 3)
        {
            return std::nullopt;
        }
        return Version{version, match.captured(2).isEmpty()
                                    ? std::nullopt
                                    : std::optional(match.captured(2).toInt())};
    }

    bool newer(const Version& candidate, const Version& current)
    {
        if (const auto order = ::QVersionNumber::compare(candidate.number, current.number))
        {
            return order > 0;
        }
        return current.preview && (!candidate.preview || *candidate.preview > *current.preview);
    }

    std::optional<meowlive::desktop::update::Chunk> digest(const ::QJsonObject& value,
                                                           std::int64_t maximum)
    {
        static const ::QRegularExpression hash(QStringLiteral(R"(\A[0-9a-f]{64}\z)"));
        const auto text = value.value(QStringLiteral("sha256")).toString();
        const auto size = value.value(QStringLiteral("size")).toInteger(-1);
        if (size <= 0 || size > maximum || !hash.match(text).hasMatch())
        {
            return std::nullopt;
        }
        return meowlive::desktop::update::Chunk{::QByteArray::fromHex(text.toLatin1()), size};
    }

    ::QByteArray base64(const ::QJsonObject& object, const ::QString& key)
    {
        return ::QByteArray::fromBase64(object.value(key).toString().toLatin1(),
                                        ::QByteArray::AbortOnBase64DecodingErrors);
    }
}

export namespace meowlive::desktop::update
{
    bool valid_tag(const ::QString& tag)
    {
        return parse_version(tag).has_value();
    }

    std::expected<meowlive::desktop::update::Manifest, ::QString>
    verify_manifest(const ::QByteArray& envelope, const ::QByteArray& public_key,
                    const ::QString& expected_tag, const ::QString& installed_version)
    {
        const auto input = ::QJsonDocument::fromJson(envelope).object();
        const auto payload = base64(input, QStringLiteral("payload"));
        const auto signature = base64(input, QStringLiteral("signature"));
        if (public_key.size() != crypto_sign_PUBLICKEYBYTES ||
            signature.size() != crypto_sign_BYTES || payload.isEmpty() || ::sodium_init() < 0 ||
            ::crypto_sign_verify_detached(
                reinterpret_cast<const unsigned char*>(signature.constData()),
                reinterpret_cast<const unsigned char*>(payload.constData()),
                static_cast<unsigned long long>(payload.size()),
                reinterpret_cast<const unsigned char*>(public_key.constData())) != 0)
        {
            return std::unexpected(QStringLiteral("更新签名无效"));
        }
        const auto data = ::QJsonDocument::fromJson(payload).object();
        const auto tag = data.value(QStringLiteral("tag")).toString();
        const auto version = parse_version(tag);
        const auto installed = parse_version(installed_version);
        if (!version || !installed || !newer(*version, *installed) || tag != expected_tag ||
            data.value(QStringLiteral("schema")).toInt() != 1 ||
            data.value(QStringLiteral("target")) != QStringLiteral("windows-x64"))
        {
            return std::unexpected(QStringLiteral("更新版本或目标平台无效"));
        }
        static const ::QRegularExpression filename(
            QStringLiteral(R"(\A[A-Za-z0-9_ .-]{1,160}-setup\.exe\z)"));
        const auto installer = data.value(QStringLiteral("installer")).toString();
        auto file = digest(data, 2'147'483'647);
        const auto parts = data.value(QStringLiteral("chunks")).toArray();
        if (!file || !filename.match(installer).hasMatch() || parts.isEmpty() ||
            parts.size() > 8192)
        {
            return std::unexpected(QStringLiteral("安装包元数据无效"));
        }
        meowlive::desktop::update::Manifest result{tag, installer, std::move(*file), {}};
        std::int64_t size{};
        for (const auto& part : parts)
        {
            auto chunk = digest(part.toObject(), 1'048'576);
            if (!chunk || chunk->size > result.file.size - size)
            {
                return std::unexpected(QStringLiteral("更新分块大小或摘要无效"));
            }
            size += chunk->size;
            result.chunks.push_back(std::move(*chunk));
        }
        if (size != result.file.size)
        {
            return std::unexpected(QStringLiteral("更新清单的总大小不一致"));
        }
        return result;
    }
}
