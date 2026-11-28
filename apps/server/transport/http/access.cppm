module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.access;

import meowlive.standard;

import meowlive.server.configuration;
export import meowlive.server.transport.http_response;

namespace
{
    constexpr std::string_view whitespace{" \t\n\r\f\v"};

    [[noreturn]] void invalid_credential(std::string_view role, std::string_view source)
    {
        throw std::runtime_error("认证凭据无效 [auth." + std::string(role) + "_token_" +
                                 std::string(source) + "]");
    }

    std::string load_digest(std::string_view role, const std::string& name,
                            const std::filesystem::path& file)
    {
        const auto* env = name.empty() ? nullptr : std::getenv(name.c_str());
        std::string value;
        if (env)
        {
            value = env;
        }
        else if (!file.empty())
        {
            std::ifstream input(file, std::ios::binary);
            if (!input)
            {
                invalid_credential(role, "file");
            }
            value.resize(1025);
            input.read(value.data(), static_cast<std::streamsize>(value.size()));
            if (input.bad() || input.gcount() == static_cast<std::streamsize>(value.size()))
            {
                invalid_credential(role, "file");
            }
            value.resize(static_cast<std::size_t>(input.gcount()));
        }
        const auto first = value.find_first_not_of(whitespace);
        if (first == std::string::npos)
        {
            invalid_credential(role, env || file.empty() ? "env" : "file");
        }
        const auto token =
            std::string_view(value).substr(first, value.find_last_not_of(whitespace) - first + 1);
        if (token.size() < 32 || token.size() > 512 ||
            !std::ranges::all_of(token,
                                 [](unsigned char byte)
                                 {
                                     return byte >= 0x21 && byte <= 0x7e;
                                 }))
        {
            invalid_credential(role, env ? "env" : "file");
        }
        return drogon::utils::getSha256(token.data(), token.size());
    }

    bool equal_digest(std::string_view first, std::string_view second)
    {
        unsigned difference{};
        for (std::size_t index = 0; index < 64; ++index)
        {
            difference |= static_cast<unsigned char>(first[index]) ^
                          static_cast<unsigned char>(second[index]);
        }
        return difference == 0;
    }

    std::string digest(std::string_view token)
    {
        return drogon::utils::getSha256(token.data(), token.size());
    }
}

export namespace meowlive::server::transport
{
    enum class LoginResult
    {
        Success,
        Disabled,
        Invalid,
        RateLimited
    };

    struct LoginSession
    {
        meowlive::server::transport::LoginResult result;
        std::string token{};
        unsigned expires_in_seconds{};
    };

    class Access
    {
    public:
        explicit Access(const meowlive::server::ServerConfig& config)
            : origins_(config.allowed_origins), enabled_(config.auth.enabled),
              lifetime_seconds_(config.auth.session_lifetime_seconds)
        {
            if (!enabled_)
            {
                return;
            }
            admin_ =
                load_digest("admin", config.auth.admin_token_env, config.auth.admin_token_file);
            device_ =
                load_digest("device", config.auth.device_token_env, config.auth.device_token_file);
            if (equal_digest(admin_, device_))
            {
                throw std::runtime_error("管理员与设备凭据必须相互独立 [auth]");
            }
        }

        bool enabled() const
        {
            return enabled_;
        }

        bool permits_origin(std::string_view origin) const
        {
            return std::ranges::find(origins_, origin) != origins_.end();
        }

        bool is_device(std::string_view token) const
        {
            return enabled_ && !token.empty() && equal_digest(device_, digest(token));
        }

        meowlive::server::transport::LoginSession login(std::string_view credential)
        {
            if (!enabled_)
            {
                return {meowlive::server::transport::LoginResult::Disabled};
            }
            const std::scoped_lock lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            while (!failures_.empty() && now - failures_.front() >= std::chrono::seconds(60))
            {
                failures_.pop_front();
            }
            if (failures_.size() >= 5)
            {
                return {meowlive::server::transport::LoginResult::RateLimited};
            }
            if (!equal_digest(admin_, digest(credential)))
            {
                failures_.push_back(now);
                return {meowlive::server::transport::LoginResult::Invalid};
            }
            auto token = drogon::utils::secureRandomString(64);
            failures_.clear();
            prune_sessions(now);
            if (sessions_.size() == 16)
            {
                sessions_.pop_front();
            }
            sessions_.push_back({digest(token), now + std::chrono::seconds(lifetime_seconds_)});
            return {meowlive::server::transport::LoginResult::Success, std::move(token),
                    lifetime_seconds_};
        }

        bool is_admin_session(std::string_view token)
        {
            if (token.empty())
            {
                return false;
            }
            const auto supplied = digest(token);
            const std::scoped_lock lock(mutex_);
            prune_sessions(std::chrono::steady_clock::now());
            bool matched{};
            for (const auto& session : sessions_)
            {
                matched |= equal_digest(session.token_digest, supplied);
            }
            return matched;
        }

        bool revoke(std::string_view token)
        {
            if (token.empty())
            {
                return false;
            }
            const auto supplied = digest(token);
            const std::scoped_lock lock(mutex_);
            prune_sessions(std::chrono::steady_clock::now());
            return std::erase_if(sessions_,
                                 [&](const Session& session)
                                 {
                                     return equal_digest(session.token_digest, supplied);
                                 }) != 0;
        }

    private:
        struct Session
        {
            std::string token_digest;
            std::chrono::steady_clock::time_point expires_at;
        };

        void prune_sessions(std::chrono::steady_clock::time_point now)
        {
            while (!sessions_.empty() && sessions_.front().expires_at <= now)
            {
                sessions_.pop_front();
            }
        }

        std::vector<std::string> origins_;
        bool enabled_;
        unsigned lifetime_seconds_;
        std::string admin_;
        std::string device_;
        std::mutex mutex_;
        std::deque<Session> sessions_;
        std::deque<std::chrono::steady_clock::time_point> failures_;
    };

    drogon::HttpResponsePtr authentication_required()
    {
        return meowlive::server::transport::error_response(
            drogon::k401Unauthorized, "authentication_required", "需要有效的 Bearer 凭据");
    }

    std::string_view bearer(const drogon::HttpRequestPtr& request)
    {
        constexpr std::string_view prefix{"Bearer "};
        const std::string_view value = request->getHeader("authorization");
        if (!value.starts_with(prefix) || value.size() == prefix.size())
        {
            return {};
        }
        const auto token = value.substr(prefix.size());
        return token.find_first_of(whitespace) == std::string_view::npos ? token
                                                                         : std::string_view{};
    }

    drogon::HttpResponsePtr origin_rejection(const drogon::HttpRequestPtr& request,
                                             const meowlive::server::transport::Access& access)
    {
        if (request->headers().contains("origin") &&
            !access.permits_origin(request->getHeader("origin")))
        {
            return meowlive::server::transport::error_response(
                drogon::k403Forbidden, "invalid_origin", "不允许此请求来源");
        }
        return {};
    }

    drogon::HttpResponsePtr device_rejection(const drogon::HttpRequestPtr& request,
                                             const meowlive::server::transport::Access& access)
    {
        if (auto response = meowlive::server::transport::origin_rejection(request, access))
        {
            return response;
        }
        if (!access.enabled())
        {
            return {};
        }
        if (!access.is_device(meowlive::server::transport::bearer(request)))
        {
            return meowlive::server::transport::authentication_required();
        }
        return {};
    }
}
