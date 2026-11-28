#include <brotli/encode.h>
#include <drogon/WebSocketController.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <zlib.h>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;
import meowlive.adapters.bilibili.live_source;
import meowlive.adapters.bilibili.packet;
import meowlive.adapters.bilibili.signing;
import meowlive.application.live;
import meowlive.domain.live_event;
import meowlive.protocol.json;

namespace
{
    namespace app = meowlive::application;
    namespace bili = meowlive::adapters::bilibili;
    namespace domain = meowlive::domain;
    namespace json = meowlive::protocol::json;

    const app::LiveCredentials credentials{17, "fixture-id", "fixture-secret", "fixture-code"};
    constexpr std::string_view chat =
        R"({"cmd":"LIVE_OPEN_PLATFORM_DM","data":{"room_id":42,"msg_id":"chat-1","uname":"测试观众","timestamp":1700000000,"msg":"你好","open_id":"viewer-1"}})";
    constexpr std::string_view gift =
        R"({"cmd":"LIVE_OPEN_PLATFORM_SEND_GIFT","data":{"room_id":42,"msg_id":"gift-1","uname":"Viewer","timestamp":1700000001,"gift_name":"Flower","gift_num":2,"uid":9,"open_id":"opaque","price":10,"paid":false,"fans_medal_level":3}})";
    constexpr std::string_view sc =
        R"({"cmd":"LIVE_OPEN_PLATFORM_SUPER_CHAT","data":{"room_id":42,"message_id":701,"uname":"Viewer","timestamp":1700000002,"message":"Hello","rmb":30,"start_time":1700000002,"end_time":1700000062}})";
    constexpr std::string_view enter =
        R"({"cmd":"LIVE_OPEN_PLATFORM_LIVE_ROOM_ENTER","data":{"room_id":42,"msg_id":"enter-1","uname":"Viewer","timestamp":1700000003}})";
    constexpr std::string_view ended =
        R"({"cmd":"LIVE_OPEN_PLATFORM_INTERACTION_END","data":{"game_id":"game-fixture"}})";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    std::string compress_zlib(std::string_view bytes)
    {
        auto output_size = ::compressBound(static_cast<uLong>(bytes.size()));
        std::string output(output_size, '\0');
        require(::compress(reinterpret_cast<Bytef*>(output.data()), &output_size,
                           reinterpret_cast<const Bytef*>(bytes.data()),
                           static_cast<uLong>(bytes.size())) == Z_OK,
                "zlib fixture compression");
        output.resize(output_size);
        return output;
    }

    std::string compress_brotli(std::string_view bytes)
    {
        auto output_size = ::BrotliEncoderMaxCompressedSize(bytes.size());
        std::string output(output_size, '\0');
        require(::BrotliEncoderCompress(4, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, bytes.size(),
                                        reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                        &output_size,
                                        reinterpret_cast<std::uint8_t*>(output.data())) != 0,
                "Brotli fixture compression");
        output.resize(output_size);
        return output;
    }

    void protocol()
    {
        const auto signed_headers = bili::signed_headers(
            credentials.access_key_id, credentials.access_key_secret,
            R"({"app_id":17,"code":"fixture-code"})", 1'700'000'000, "nonce-1");
        require(signed_headers &&
                    (*signed_headers)[1] ==
                        "x-bili-content-md5: f8090b585a28908d4a6f0a982bebdf57" &&
                    (*signed_headers)[6] ==
                        "Authorization: "
                        "e569b770fcff496607a79428eee028dc0e2ad20d43a42d5579ca9165194d2603",
                "MD5 and HMAC signing vector");
        require(!bili::signed_headers("a\r\nmalformed", "s", "{}", 0, "n"),
                "reject malformed signature header");

        const auto chat_packet = bili::encode_packet(5, chat);
        const auto zlib = bili::encode_packet(
            5, compress_zlib(bili::encode_packet(5, gift) + bili::encode_packet(5, sc)), 2);
        const auto brotli =
            bili::encode_packet(5, compress_brotli(bili::encode_packet(5, enter)), 3);
        const auto response = bili::decode_frame(
            bili::encode_packet(8, R"({"code":0})") + bili::encode_packet(3, std::string(4, '\0')) +
                chat_packet + zlib + brotli + bili::encode_packet(5, ended) + chat_packet,
            42, 17, "game-fixture");
        require(response && response->authorized == true && response->heartbeat &&
                    response->interaction_ended && response->events.size() == 4,
                "combined frame");
        const auto& dm = response->events[0];
        require(dm.id == "bilibili:42:chat-1" && dm.source == "bilibili" &&
                    dm.viewer == "测试观众" && dm.occurred_at_ms == 1'700'000'000'000ULL &&
                    dm.viewer_identity &&
                    dm.viewer_identity->identity_namespace == "bilibili:app:17" &&
                    std::get<domain::Chat>(dm.kind).text == "你好",
                "chat envelope");
        const auto& gift_event = response->events[1];
        require(gift_event.viewer_identity && gift_event.viewer_identity->external_id == "9" &&
                    gift_event.gift_metadata && gift_event.gift_metadata->paid == false &&
                    gift_event.gift_metadata->price == 10 &&
                    std::get<domain::Gift>(gift_event.kind).count == 2,
                "gift metadata");
        const auto& superchat = response->events[2];
        require(superchat.id == "bilibili:42:sc:701" &&
                    std::get<domain::SuperChat>(superchat.kind).end_at_ms == 1'700'000'062'000ULL &&
                    std::holds_alternative<domain::RoomEnter>(response->events[3].kind),
                "SC and entry");
        require(bili::decode_frame(chat_packet, 21, 17, "game-fixture")->events.empty(),
                "room filter");
        require(!bili::decode_frame(chat_packet.substr(1), 42, 17, "game-fixture"),
                "invalid header");
        require(!bili::decode_frame(
                    bili::encode_packet(5, compress_brotli(std::string(1024 * 1024 + 1, '\0')), 3),
                    42, 17, "game-fixture"),
                "decompression limit");
        require(!bili::decode_frame(bili::encode_packet(5, compress_zlib(zlib) + "tail", 2), 42, 17,
                                    "game-fixture"),
                "compressed message trailing data");
    }

    struct Scenario
    {
        std::atomic<unsigned> starts{};
        std::atomic<unsigned> app_heartbeats{};
        std::atomic<unsigned> websocket_heartbeats{};
        std::atomic<unsigned> ends{};
        std::atomic<bool> drop_heartbeats{};
        std::atomic<bool> reject_authority{};
        std::atomic<bool> corrupt_start{};
        std::atomic<bool> reject_end{};
        std::atomic<bool> lost{};
        std::atomic<bool> end_saw_lost{};
        std::uint16_t port{};
    };

    class SocketFixture final : public drogon::WebSocketController<SocketFixture, false>
    {
    public:
        explicit SocketFixture(Scenario& scenario) : scenario_(scenario)
        {
        }

        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/socket");
        WS_PATH_LIST_END

        void handleNewMessage(const drogon::WebSocketConnectionPtr& socket, std::string&& message,
                              const drogon::WebSocketMessageType& type) override
        {
            if (type != drogon::WebSocketMessageType::Binary)
            {
                return;
            }
            if (message == bili::encode_packet(7, "auth-fixture"))
            {
                const auto response =
                    scenario_.reject_authority.load()
                        ? bili::encode_packet(8, R"({"code":1})")
                        : bili::encode_packet(8, R"({"code":0})") + bili::encode_packet(5, chat);
                socket->send(response, drogon::WebSocketMessageType::Binary);
            }
            else if (message == bili::encode_packet(2))
            {
                ++scenario_.websocket_heartbeats;
                if (!scenario_.drop_heartbeats.load())
                {
                    socket->send(bili::encode_packet(3, std::string(4, '\0')),
                                 drogon::WebSocketMessageType::Binary);
                }
            }
        }

        void handleNewConnection(const drogon::HttpRequestPtr&,
                                 const drogon::WebSocketConnectionPtr&) override
        {
        }
        void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override
        {
        }

    private:
        Scenario& scenario_;
    };

    bool matching_signature(const drogon::HttpRequestPtr& request)
    {
        const auto& timestamp = request->getHeader("x-bili-timestamp");
        std::uint64_t seconds{};
        const auto end = timestamp.data() + timestamp.size();
        const auto parsed = std::from_chars(timestamp.data(), end, seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
        {
            return false;
        }
        const auto& nonce = request->getHeader("x-bili-signature-nonce");
        const auto expected =
            bili::signed_headers(credentials.access_key_id, credentials.access_key_secret,
                                 request->body(), seconds, nonce);
        return nonce.size() == 32 && expected &&
               request->getHeader("authorization") ==
                   (*expected)[6].substr(std::string_view{"Authorization: "}.size()) &&
               request->getHeader("x-bili-content-md5") ==
                   (*expected)[1].substr(std::string_view{"x-bili-content-md5: "}.size());
    }

    drogon::HttpResponsePtr reply(Scenario& scenario, const drogon::HttpRequestPtr& request)
    {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k400BadRequest);
        if (!matching_signature(request))
        {
            return response;
        }
        const auto payload = json::parse_object(request->body());
        if (!payload)
        {
            return response;
        }

        Json::Value body(Json::objectValue);
        body["code"] = 0;
        const auto path = request->path();
        if (path == "/v2/app/start" && (*payload)["app_id"] == 17 &&
            (*payload)["code"] == "fixture-code")
        {
            ++scenario.starts;
            body["data"]["game_info"]["game_id"] = "game-fixture";
            body["data"]["anchor_info"]["room_id"] =
                scenario.corrupt_start.load() ? Json::Value("bad-room") : Json::Value(42);
            body["data"]["websocket_info"]["auth_body"] = "auth-fixture";
            body["data"]["websocket_info"]["wss_link"].append("ws://example.invalid/socket");
            body["data"]["websocket_info"]["wss_link"].append(
                "ws://127.0.0.1:" + std::to_string(scenario.port) + "/socket");
        }
        else if (path == "/v2/app/heartbeat" && (*payload)["game_id"] == "game-fixture")
        {
            ++scenario.app_heartbeats;
        }
        else if (path == "/v2/app/end" && (*payload)["app_id"] == 17 &&
                 (*payload)["game_id"] == "game-fixture")
        {
            ++scenario.ends;
            scenario.end_saw_lost.store(scenario.lost.load());
            if (scenario.reject_end.load())
            {
                body["code"] = 4001;
            }
        }
        else
        {
            return response;
        }
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("application/json");
        response->setBody(json::serialize(body));
        return response;
    }

    class Fixture
    {
    public:
        Fixture()
        {
            auto& server = drogon::app();
            server.disableSigtermHandling()
                .setThreadNum(1)
                .enableGzip(false)
                .setLogLevel(trantor::Logger::kError)
                .setUploadPath(
                    drogon::utils::fromNativePath(std::filesystem::temp_directory_path().native()))
                .addListener("127.0.0.1", 0);
            server.registerController(std::make_shared<SocketFixture>(scenario));
            server.registerHandlerViaRegex(
                "/v2/app/.*",
                [this](const drogon::HttpRequestPtr& request,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback)
                {
                    callback(reply(scenario, request));
                },
                {drogon::Post});
            std::promise<void> ready;
            auto started = ready.get_future();
            server.registerBeginningAdvice(
                [this, &ready]
                {
                    const auto listeners = drogon::app().getListeners();
                    scenario.port = listeners.empty() ? 0 : listeners.front().toPort();
                    ready.set_value();
                });
            thread_ = std::thread(
                []
                {
                    drogon::app().run();
                });
            started.get();
        }

        ~Fixture()
        {
            drogon::app().quit();
            thread_.join();
        }

        Scenario scenario;

    private:
        std::thread thread_;
    };

    void connection()
    {
        using namespace std::chrono_literals;
        Fixture fixture;
        auto& scenario = fixture.scenario;
        require(scenario.port != 0, "loopback listener");
        const bili::ConnectionOptions options{.api_base = "http://127.0.0.1:" +
                                                          std::to_string(scenario.port),
                                              .request_timeout = 2s,
                                              .connect_timeout = 2s,
                                              .receive_timeout = 350ms,
                                              .app_heartbeat_interval = 40ms,
                                              .websocket_heartbeat_interval = 40ms};
        std::stop_source stop;
        std::jthread request_stop(
            [&](std::stop_token end)
            {
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!end.stop_requested() && std::chrono::steady_clock::now() < deadline)
                {
                    if (scenario.app_heartbeats.load() > 0 &&
                        scenario.websocket_heartbeats.load() > 0)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(5ms);
                }
                stop.request_stop();
            });
        std::string room;
        std::string event_id;
        const auto connected = [&](std::string id)
        {
            room = std::move(id);
        };
        const auto event = [&](domain::LiveEvent notification)
        {
            event_id = std::move(notification.id);
        };
        const auto lost = [&]
        {
            scenario.lost.store(true);
        };

        const auto stopped =
            bili::run_connection(credentials, stop.get_token(), connected, event, lost, options);
        request_stop.request_stop();
        request_stop.join();
        require(stopped == app::LiveSourceFailure::Stopped && scenario.starts == 1 &&
                    scenario.app_heartbeats > 0 && scenario.websocket_heartbeats > 0 &&
                    scenario.ends == 1 && scenario.end_saw_lost && room == "42" &&
                    event_id == "bilibili:42:chat-1",
                "signed loopback session and both heartbeats");

        scenario.lost = false;
        scenario.drop_heartbeats = true;
        const auto disconnected =
            bili::run_connection(credentials, {}, connected, event, lost, options);
        require(disconnected == app::LiveSourceFailure::Disconnected && scenario.starts == 2 &&
                    scenario.ends == 2 && scenario.end_saw_lost,
                "missing heartbeat expires session");

        room.clear();
        scenario.lost = false;
        scenario.reject_authority = true;
        const auto rejected =
            bili::run_connection(credentials, {}, connected, event, lost, options);
        require(rejected == app::LiveSourceFailure::Authorization && scenario.starts == 3 &&
                    scenario.ends == 3 && !scenario.end_saw_lost && room.empty(),
                "failed socket authority ends started project");

        scenario.corrupt_start = true;
        const auto malformed =
            bili::run_connection(credentials, {}, connected, event, lost, options);
        require(malformed == app::LiveSourceFailure::Authorization && scenario.starts == 4 &&
                    scenario.ends == 4 && room.empty(),
                "known game id recovered from malformed start");

        scenario.reject_end = true;
        const auto cleanup = bili::run_connection(credentials, {}, connected, event, lost, options);
        require(cleanup == app::LiveSourceFailure::Cleanup && scenario.starts == 5 &&
                    scenario.ends == 5,
                "end failure prevents a fresh start");

        scenario.reject_authority = false;
        scenario.corrupt_start = false;
        scenario.reject_end = false;
        scenario.lost = false;
        bool callback_error_propagated{};
        try
        {
            bili::run_connection(
                credentials, {}, connected,
                [](domain::LiveEvent)
                {
                    throw std::logic_error("event callback fixture");
                },
                [&]
                {
                    lost();
                    throw std::runtime_error("lost callback fixture");
                },
                options);
        }
        catch (const std::logic_error& error)
        {
            callback_error_propagated = std::string_view(error.what()) == "event callback fixture";
        }
        require(callback_error_propagated && scenario.starts == 6 && scenario.ends == 6 &&
                    scenario.end_saw_lost,
                "callback exception propagates after ending the project");
    }
}

int main()
{
    try
    {
        protocol();
        connection();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
