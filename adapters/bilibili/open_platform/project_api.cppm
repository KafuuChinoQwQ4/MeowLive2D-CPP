module;

#include <curl/curl.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.project_api;

import meowlive.standard;
import meowlive.adapters.bilibili.signing;
import meowlive.adapters.bilibili.transport;
import meowlive.adapters.http.response_body;
import meowlive.application.live;
import meowlive.protocol.json;

export namespace meowlive::adapters::bilibili
{
    struct Project
    {
        std::string game_id;
        std::string auth_body;
        std::vector<std::string> urls;
        std::uint64_t room{};
    };
}

namespace
{
    namespace app = meowlive::application;
    namespace bili = meowlive::adapters::bilibili;
    namespace json = meowlive::protocol::json;

    constexpr std::size_t max_response_bytes = 256 * 1024;

    bool retryable_api_error(Json::Int64 code)
    {
        return code == 4003 || code == 4004 || (code >= 5000 && code <= 5003) || code == 7001;
    }

    const Json::Value& member(const Json::Value& value, const char* key)
    {
        static const Json::Value missing;
        return value.isObject() ? value[key] : missing;
    }
}

export namespace meowlive::adapters::bilibili
{
    class ProjectApi
    {
    public:
        ProjectApi(const meowlive::application::LiveCredentials& credentials,
                   const std::string& base_url, std::chrono::milliseconds timeout)
            : credentials_(credentials), base_url_(base_url), timeout_(timeout)
        {
        }

        std::expected<meowlive::adapters::bilibili::Project,
                      meowlive::application::LiveSourceFailure>
        start()
        {
            Json::Value request(Json::objectValue);
            request["app_id"] = Json::UInt64(credentials_.app_id);
            request["code"] = credentials_.identity_code;

            // Start may be accepted even if the caller cancels. Wait for the bounded result
            // so a returned game id can always be ended.
            auto response = post("/v2/app/start", request, {});
            if (!response)
            {
                return std::unexpected(response.error());
            }
            const auto code = (*response)["code"].asInt64();
            if (code)
            {
                return std::unexpected(retryable_api_error(code)
                                           ? app::LiveSourceFailure::Disconnected
                                           : app::LiveSourceFailure::Authorization);
            }
            const auto& data = member(*response, "data");
            const auto& game = member(member(data, "game_info"), "game_id");
            const auto& websocket = member(data, "websocket_info");
            const auto& auth = member(websocket, "auth_body");
            const auto& urls = member(websocket, "wss_link");
            const auto& room = member(member(data, "anchor_info"), "room_id");

            bili::Project result;
            if (game.isString())
            {
                result.game_id = game.asString();
            }
            if (result.game_id.empty() || result.game_id.size() > 128)
            {
                return std::unexpected(app::LiveSourceFailure::Cleanup);
            }
            if (!auth.isString() || auth.asString().empty() || auth.asString().size() > 64 * 1024 ||
                !urls.isArray() || urls.empty() || urls.size() > 8 ||
                (room.type() != Json::intValue && room.type() != Json::uintValue) ||
                !room.isUInt64() || room.asUInt64() == 0)
            {
                return std::unexpected(end(result.game_id) ? app::LiveSourceFailure::Authorization
                                                           : app::LiveSourceFailure::Cleanup);
            }
            result.auth_body = auth.asString();
            result.room = room.asUInt64();
            for (const auto& url : urls)
            {
                if (url.isString())
                {
                    result.urls.push_back(url.asString());
                }
            }
            return result;
        }

        bool heartbeat(const std::string& game_id, std::stop_token stop)
        {
            Json::Value request(Json::objectValue);
            request["game_id"] = game_id;
            const auto response = post("/v2/app/heartbeat", request, stop);
            return response && (*response)["code"].asInt64() == 0;
        }

        bool end(const std::string& game_id)
        {
            Json::Value request(Json::objectValue);
            request["app_id"] = Json::UInt64(credentials_.app_id);
            request["game_id"] = game_id;
            const auto response = post("/v2/app/end", request, {});
            return response &&
                   ((*response)["code"].asInt64() == 0 || (*response)["code"].asInt64() == 7003);
        }

    private:
        std::expected<Json::Value, meowlive::application::LiveSourceFailure>
        post(std::string_view path, const Json::Value& request, std::stop_token stop)
        {
            const auto transport_error = std::unexpected(app::LiveSourceFailure::Disconnected);
            const auto unknown =
                std::unexpected(path == "/v2/app/start" ? app::LiveSourceFailure::Cleanup
                                                        : app::LiveSourceFailure::Disconnected);
            const auto body = json::serialize(request);
            const auto utc = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            const auto nonce = bili::signing_nonce();
            if (!nonce || utc < 0)
            {
                return transport_error;
            }
            const auto headers =
                bili::signed_headers(credentials_.access_key_id, credentials_.access_key_secret,
                                     body, static_cast<std::uint64_t>(utc), *nonce);
            if (!headers)
            {
                return std::unexpected(app::LiveSourceFailure::Authorization);
            }
            std::unique_ptr<::curl_slist, decltype(&::curl_slist_free_all)> list(
                nullptr, ::curl_slist_free_all);
            for (const auto& header : *headers)
            {
                auto* next = ::curl_slist_append(list.get(), header.c_str());
                if (!next)
                {
                    throw std::bad_alloc{};
                }
                list.release();
                list.reset(next);
            }

            meowlive::adapters::http::ResponseBody response(max_response_bytes);
            const auto url = std::string(base_url_).append(path);
            bili::Transfer transfer;
            if (!transfer.configure(url, "https,http", timeout_) ||
                ::curl_easy_setopt(transfer.easy(), CURLOPT_HTTPHEADER, list.get()) != CURLE_OK ||
                ::curl_easy_setopt(transfer.easy(), CURLOPT_POSTFIELDSIZE,
                                   static_cast<long>(body.size())) != CURLE_OK ||
                ::curl_easy_setopt(transfer.easy(), CURLOPT_POSTFIELDS, body.data()) != CURLE_OK ||
                ::curl_easy_setopt(transfer.easy(), CURLOPT_WRITEFUNCTION,
                                   meowlive::adapters::http::ResponseBody::receive) != CURLE_OK ||
                ::curl_easy_setopt(transfer.easy(), CURLOPT_WRITEDATA, &response) != CURLE_OK)
            {
                return transport_error;
            }
            const auto code = transfer.perform(stop);
            response.rethrow_failure();
            if (code != CURLE_OK)
            {
                switch (code)
                {
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_CONNECT:
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_FAILED_INIT:
                    return transport_error;
                default:
                    return unknown;
                }
            }
            long status{};
            ::curl_easy_getinfo(transfer.easy(), CURLINFO_RESPONSE_CODE, &status);
            if (status < 200 || status >= 300)
            {
                if (status == 429)
                {
                    return transport_error;
                }
                if (status >= 500)
                {
                    return unknown;
                }
                return std::unexpected(app::LiveSourceFailure::Authorization);
            }
            auto parsed = json::parse_object(response.bytes());
            if (!parsed ||
                ((*parsed)["code"].type() != Json::intValue &&
                 (*parsed)["code"].type() != Json::uintValue) ||
                !(*parsed)["code"].isInt64())
            {
                return unknown;
            }
            return std::move(*parsed);
        }

        const meowlive::application::LiveCredentials& credentials_;
        const std::string& base_url_;
        std::chrono::milliseconds timeout_;
    };
}
