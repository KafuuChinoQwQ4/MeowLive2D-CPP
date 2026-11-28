#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <sodium.h>

#include <array>
#include <cstdio>
#include <stdexcept>

import meowlive.desktop.update.manifest;

namespace
{
    void require(bool satisfied)
    {
        if (!satisfied)
        {
            throw std::runtime_error("Windows update manifest contract failed");
        }
    }
}

int main()
{
    try
    {
        require(::sodium_init() >= 0);
        std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
        std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
        std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret_key{};
        ::crypto_sign_seed_keypair(public_key.data(), secret_key.data(), seed.data());
        const ::QByteArray key(reinterpret_cast<const char*>(public_key.data()), public_key.size());
        const auto bytes = QByteArrayLiteral("synthetic installer");
        const auto hash = ::QCryptographicHash::hash(bytes, ::QCryptographicHash::Sha256);
        const ::QJsonObject checksum{{QStringLiteral("size"), bytes.size()},
                                     {QStringLiteral("sha256"), QString::fromLatin1(hash.toHex())}};
        ::QJsonObject manifest(checksum);
        manifest.insert(QStringLiteral("schema"), 1);
        manifest.insert(QStringLiteral("tag"), QStringLiteral("v0.1.4"));
        manifest.insert(QStringLiteral("target"), QStringLiteral("windows-x64"));
        manifest.insert(QStringLiteral("installer"), QStringLiteral("MeowLive2D-setup.exe"));
        manifest.insert(QStringLiteral("chunks"), ::QJsonArray{checksum});
        const auto sign = [&](const ::QJsonObject& input)
        {
            const auto payload = ::QJsonDocument(input).toJson(::QJsonDocument::Compact);
            std::array<unsigned char, crypto_sign_BYTES> signature{};
            require(::crypto_sign_detached(
                        signature.data(), nullptr,
                        reinterpret_cast<const unsigned char*>(payload.constData()),
                        static_cast<unsigned long long>(payload.size()), secret_key.data()) == 0);
            return ::QJsonObject{
                {QStringLiteral("payload"), QString::fromLatin1(payload.toBase64())},
                {QStringLiteral("signature"),
                 QString::fromLatin1(
                     ::QByteArray(reinterpret_cast<const char*>(signature.data()), signature.size())
                         .toBase64())}};
        };
        const auto verify = [&](const ::QJsonObject& envelope, const ::QString& current)
        {
            return meowlive::desktop::update::verify_manifest(
                ::QJsonDocument(envelope).toJson(::QJsonDocument::Compact), key,
                QStringLiteral("v0.1.4"), current);
        };

        auto signed_manifest = sign(manifest);
        const auto verified = verify(signed_manifest, QStringLiteral("0.1.3"));
        require(verified && verified->file.digest == hash && verified->chunks.size() == 1);
        require(
            verify(signed_manifest, QStringLiteral("v0.1.4-windows-preview.20261124")).has_value());
        require(!verify(signed_manifest, QStringLiteral("v0.1.4")));
        signed_manifest[QStringLiteral("payload")] = QStringLiteral("YQ==");
        require(!verify(signed_manifest, QStringLiteral("0.1.3")));

        manifest[QStringLiteral("installer")] = QStringLiteral("../unsafe-setup.exe");
        require(!verify(sign(manifest), QStringLiteral("0.1.3")));
        manifest[QStringLiteral("installer")] = QStringLiteral("MeowLive2D-setup.exe");
        manifest[QStringLiteral("chunks")] = ::QJsonArray{checksum, checksum};
        require(!verify(sign(manifest), QStringLiteral("0.1.3")));
        std::puts("Windows update manifest contract passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fputs(error.what(), stderr);
        return 1;
    }
}
