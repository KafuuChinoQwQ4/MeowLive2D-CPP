module;

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.signing;

import meowlive.standard;

namespace
{
    std::string hex_lower(std::span<const unsigned char> bytes)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const auto byte : bytes)
        {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0xf]);
        }
        return result;
    }

#if defined(_WIN32)
    bool digest(const wchar_t* algorithm, std::string_view body, std::string_view key,
                std::span<unsigned char> result)
    {
        BCRYPT_ALG_HANDLE provider{};
        if (::BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr,
                                          key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        {
            return false;
        }
        const auto input = [](std::string_view text)
        {
            return reinterpret_cast<unsigned char*>(const_cast<char*>(text.data()));
        };
        const auto status = ::BCryptHash(provider, input(key), static_cast<ULONG>(key.size()),
                                         input(body), static_cast<ULONG>(body.size()),
                                         result.data(), static_cast<ULONG>(result.size()));
        ::BCryptCloseAlgorithmProvider(provider, 0);
        return status >= 0;
    }
#endif
}

export namespace meowlive::adapters::bilibili
{
    std::optional<std::string> signing_nonce()
    {
        std::array<unsigned char, 16> bytes{};
#if defined(_WIN32)
        if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
#else
        if (::RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
#endif
        {
            return std::nullopt;
        }
        return hex_lower(bytes);
    }

    std::optional<std::array<std::string, 9>>
    signed_headers(std::string_view access_key, std::string_view secret, std::string_view body,
                   std::uint64_t utc_seconds, std::string_view nonce)
    {
        const auto header_value = [](std::string_view text)
        {
            return !text.empty() && std::ranges::all_of(text,
                                                        [](unsigned char byte)
                                                        {
                                                            return byte >= '!' && byte <= '~';
                                                        });
        };
        if (access_key.size() > 256 || secret.empty() || secret.size() > 512 ||
            nonce.size() > 128 || !header_value(access_key) || !header_value(nonce))
        {
            return std::nullopt;
        }

        std::array<unsigned char, 16> md5{};
        std::array<unsigned char, 32> mac{};
#if defined(_WIN32)
        if (!digest(BCRYPT_MD5_ALGORITHM, body, {}, md5))
#else
        unsigned int length{};
        if (::EVP_Digest(body.data(), body.size(), md5.data(), &length, ::EVP_md5(), nullptr) !=
                1 ||
            length != md5.size())
#endif
        {
            return std::nullopt;
        }

        const auto md5_hex = hex_lower(md5);
        const auto timestamp = std::to_string(utc_seconds);
        const auto canonical =
            std::string("x-bili-accesskeyid:")
                .append(access_key)
                .append("\nx-bili-content-md5:")
                .append(md5_hex)
                .append("\nx-bili-signature-method:HMAC-SHA256\nx-bili-signature-nonce:")
                .append(nonce)
                .append("\nx-bili-signature-version:1.0\nx-bili-timestamp:")
                .append(timestamp);
#if defined(_WIN32)
        if (!digest(BCRYPT_SHA256_ALGORITHM, canonical, secret, mac))
#else
        if (!::HMAC(::EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                    reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(),
                    mac.data(), &length) ||
            length != mac.size())
#endif
        {
            return std::nullopt;
        }
        return std::array<std::string, 9>{"x-bili-accesskeyid: " + std::string(access_key),
                                          "x-bili-content-md5: " + md5_hex,
                                          "x-bili-signature-method: HMAC-SHA256",
                                          "x-bili-signature-nonce: " + std::string(nonce),
                                          "x-bili-signature-version: 1.0",
                                          "x-bili-timestamp: " + timestamp,
                                          "Authorization: " + hex_lower(mac),
                                          "Accept: application/json",
                                          "Content-Type: application/json"};
    }
}
