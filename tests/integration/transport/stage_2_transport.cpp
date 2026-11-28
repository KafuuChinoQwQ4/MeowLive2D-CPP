#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.json;
import meowlive.tests.server_process;
import meowlive.tests.stage_2_startup;
import meowlive.tests.transport_client;

namespace
{
    constexpr std::string_view control_path{"/ws/control"};
    constexpr std::string_view health_path{"/api/health"};
    constexpr std::string_view status_path{"/api/status"};
    constexpr std::string_view session_path{"/api/admin/session"};
    constexpr std::string_view help{
        "meowlive-transport-integration [--rust] [--fixture PATH] [SERVER]\n"
        "With no SERVER argument, use the C++ server executable next to this test.\n"};

    using meowlive::tests::HttpResponse;
    using meowlive::tests::ServerProcess;
    using meowlive::tests::WebSocket;

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    ::Json::Value parse_object(std::string_view body)
    {
        auto value = meowlive::protocol::json::parse_object(body);
        require(value.has_value(), "Expected a JSON object");
        return std::move(*value);
    }

    ::Json::Value read_fixture(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        require(static_cast<bool>(stream), "Cannot open protocol fixture");
        ::Json::Value fixture;
        stream >> fixture;
        require(fixture["http"].isObject() && fixture["control"].isObject(),
                "Protocol fixture is missing HTTP or control samples");
        return fixture;
    }

    void expect_error(const HttpResponse& response, int status, std::string_view code)
    {
        require(response.status == status,
                std::format("Expected HTTP {}, got {}", status, response.status));
        require(parse_object(response.body)["code"].asString() == code, code);
    }

    void wait_health(const ServerProcess& process, bool paired, const ::Json::Value& fixture)
    {
        const auto& expected = fixture["http"][paired ? "GET /api/health after pairing"
                                                      : "GET /api/health before pairing"];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline)
        {
            process.assert_running();
            try
            {
                const auto actual = meowlive::tests::request(process.port(), health_path);
                if (actual.status == expected["status"].asInt() &&
                    parse_object(actual.body) == expected["body"])
                {
                    return;
                }
            }
            catch (const std::system_error&)
            {
                // The listener may still be starting.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error(std::format("Health did not reach bridge_connected={}", paired));
    }

    struct Hello
    {
        std::string session_id;
        std::string bridge_id;
        std::uint32_t generation{};
    };

    Hello hello(WebSocket& control, const ::Json::Value& fixture)
    {
        require(control.response().status == 101, "Control WebSocket upgrade failed");
        control.send(1, meowlive::protocol::json::serialize(fixture["control"]["client_hello"]));
        const auto response = parse_object(control.wait_frame(1));
        require(response["type"].asString() == "hello" && response["protocol_version"].asInt() == 3,
                "Unexpected control hello or protocol version");
        Hello answer{response["session_id"].asString(), response["bridge_id"].asString(),
                     response["generation"].asUInt()};
        require(!answer.session_id.empty() && !answer.bridge_id.empty() && answer.generation > 0,
                "Hello is missing session ID, bridge ID or generation");
        return answer;
    }

    std::string pair_path(const Hello& answer)
    {
        return "/ws/audio?session_id=" + drogon::utils::urlEncodeComponent(answer.session_id) +
               "&bridge_id=" + drogon::utils::urlEncodeComponent(answer.bridge_id);
    }

    void reject_contract(std::uint16_t port, std::string_view path, const ::Json::Value& fixture,
                         const char* sample)
    {
        const auto& expected = fixture["http"][sample];
        const WebSocket rejected(port, path);
        require(rejected.response().status == expected["status"].asInt() &&
                    parse_object(rejected.response().body) == expected["body"],
                sample);
    }

    Hello check_bridge(const ServerProcess& process, const ::Json::Value& fixture)
    {
        const auto port = process.port();
        {
            const WebSocket rejected(port, control_path, {}, "https://invalid.example");
            expect_error(rejected.response(), 403, "invalid_origin");
        }

        Hello initial;
        {
            WebSocket control(port, control_path);
            initial = hello(control, fixture);
            wait_health(process, false, fixture);
            const auto status = meowlive::tests::request(port, status_path);
            ::Json::Value expected(::Json::objectValue);
            expected["protocol_version"] = 3;
            expected["bridge_connected"] = false;
            expected["session_id"] = initial.session_id;
            expected["generation"] = static_cast<::Json::LargestInt>(initial.generation);
            expected["speeches"] = ::Json::Value(::Json::arrayValue);
            require(status.status == 200 && parse_object(status.body) == expected,
                    "Status does not match the control-only connection");
            reject_contract(port, "/ws/audio?session_id=old&bridge_id=old", fixture,
                            "GET /ws/audio invalid pair");
            reject_contract(port, control_path, fixture, "GET /ws/control occupied");
            {
                const WebSocket audio(port, pair_path(initial));
                require(audio.response().status == 101, "Audio WebSocket upgrade failed");
                wait_health(process, true, fixture);
                reject_contract(port, pair_path(initial), fixture, "GET /ws/audio occupied");
                const auto receipt =
                    meowlive::protocol::json::serialize(fixture["control"]["client_completed"]);
                control.send(1, receipt);
                control.send(1, receipt);
                control.send(9, "duplicate-receipt");
                require(control.wait_frame(10) == "duplicate-receipt",
                        "Control heartbeat failed after duplicate receipts");
                const auto after_receipt = meowlive::tests::request(port, status_path);
                require(after_receipt.status == 200,
                        "Status request failed after duplicate receipts");
                const auto body = parse_object(after_receipt.body);
                require(body["speeches"] == expected["speeches"] &&
                            body["generation"].asUInt() == initial.generation,
                        "Duplicate receipts changed the speech list or generation");
            }
            control.wait_closed();
        }
        wait_health(process, false, fixture);

        {
            WebSocket control(port, control_path);
            const auto reconnected = hello(control, fixture);
            require(reconnected.bridge_id != initial.bridge_id &&
                        reconnected.generation > initial.generation,
                    "Reconnect should advance the generation and issue a new bridge ID");
            reject_contract(port, pair_path(initial), fixture, "GET /ws/audio invalid pair");
            WebSocket audio(port, pair_path(reconnected));
            require(audio.response().status == 101, "Audio WebSocket reconnect failed");
            control.send(8, std::string_view("\x03\xe8", 2));
            audio.wait_closed();
        }
        wait_health(process, false, fixture);

        {
            WebSocket control(port, control_path);
            require(control.response().status == 101,
                    "Control upgrade failed before version check");
            auto invalid_hello = fixture["control"]["client_hello"];
            invalid_hello["protocol_version"] = 999;
            control.send(1, meowlive::protocol::json::serialize(invalid_hello));
            control.wait_closed();
        }
        Hello unpaired;
        {
            WebSocket control(port, control_path);
            unpaired = hello(control, fixture);
            control.wait_closed();
        }
        reject_contract(port, pair_path(unpaired), fixture, "GET /ws/audio invalid pair");
        {
            WebSocket control(port, control_path);
            require(control.response().status == 101,
                    "Control upgrade failed before hello timeout");
            control.wait_closed();
        }
        {
            WebSocket control(port, control_path);
            require(hello(control, fixture).session_id == initial.session_id,
                    "Recovering from an unpaired WebSocket changed the server session");
        }
        return initial;
    }

    void check_restart(const ServerProcess& process, const Hello& previous,
                       const ::Json::Value& fixture)
    {
        reject_contract(process.port(), pair_path(previous), fixture, "GET /ws/audio invalid pair");
        WebSocket control(process.port(), control_path);
        const auto answer = hello(control, fixture);
        require(answer.session_id != previous.session_id,
                "Server restart retained an old session ID");
        const WebSocket audio(process.port(), pair_path(answer));
        require(audio.response().status == 101, "Audio upgrade failed after server restart");
        wait_health(process, true, fixture);
    }

    void check_origins(std::uint16_t port)
    {
        constexpr std::string_view invalid_origin = "https://invalid.example";
        const auto allowed = meowlive::tests::request(port, health_path, "GET", {}, {},
                                                      meowlive::tests::allowed_origin);
        require(allowed.status == 200 && allowed.header("Access-Control-Allow-Origin") ==
                                             meowlive::tests::allowed_origin,
                "Allowed origin did not receive its CORS header");
        const auto rejected =
            meowlive::tests::request(port, health_path, "GET", {}, {}, invalid_origin);
        expect_error(rejected, 403, "invalid_origin");
        require(rejected.header("Access-Control-Allow-Origin").empty(),
                "Rejected origin received a CORS header");

        for (const auto origin :
             {std::string_view{}, meowlive::tests::allowed_origin, invalid_origin})
        {
            const auto response =
                meowlive::tests::request(port, session_path, "OPTIONS", {}, {}, origin);
            const auto expected_origin =
                origin == meowlive::tests::allowed_origin ? origin : std::string_view{};
            require(response.status == 200 &&
                        response.header("Access-Control-Allow-Origin") == expected_origin &&
                        response.header("Access-Control-Allow-Methods") == "GET,POST,DELETE" &&
                        response.header("Access-Control-Allow-Headers") ==
                            "content-type,authorization" &&
                        response.header("Allow") == "GET,HEAD,POST,DELETE",
                    "Unexpected OPTIONS response");
        }
    }

    std::string login(std::uint16_t port, std::string_view request_body,
                      std::string_view media_type = "application/json")
    {
        const auto answer =
            meowlive::tests::request(port, session_path, "POST", {}, request_body, {}, media_type);
        require(answer.status == 200 && answer.header("Cache-Control") == "no-store",
                "Administrator login failed or returned cacheable credentials");
        const auto body = parse_object(answer.body);
        require(body["expires_in_seconds"].asInt() == 2,
                "Unexpected administrator session duration");
        const auto token = body["token"].asString();
        require(!token.empty(), "Administrator login did not return a session token");
        return token;
    }

    void check_login_body(std::uint16_t port, const std::string& valid)
    {
        constexpr std::size_t body_limit = 16 * 1024;
        auto boundary = valid;
        boundary.resize(body_limit, ' ');
        login(port, boundary);
        login(port, std::format(R"(["{}"])", meowlive::tests::admin_credential));
        login(port, valid, "APPLICATION/JSON");
        login(port, valid, "application/vnd.meowlive+JSON; charset=\"UTF-8\"");

        boundary.push_back(' ');
        const std::array invalid_bodies{
            std::move(boundary),
            std::string{},
            std::string(R"({"token":1})"),
            std::string(R"({"token":"fixture","token":"fixture"})"),
            std::string(R"({"token":"\ud800\u0020"})"),
            std::string(R"(["fixture","extra"])"),
            std::string("\xef\xbb\xbf") + valid,
            valid + "false",
            std::format("{{\"token\":\"{}\",\"extra\":true}}", meowlive::tests::admin_credential),
        };
        for (const auto& body : invalid_bodies)
        {
            const auto rejected = meowlive::tests::request(port, session_path, "POST", {}, body);
            expect_error(rejected, 400, "invalid_request");
            require(rejected.header("Cache-Control").empty(),
                    "Malformed login used the success cache directive");
        }
        for (const std::string_view media_type :
             {"", "text/json", "application/+json", "application/json; nonsense"})
        {
            expect_error(
                meowlive::tests::request(port, session_path, "POST", {}, valid, {}, media_type),
                400, "invalid_request");
        }
    }

    void check_auth(const ServerProcess& process, const ::Json::Value& fixture)
    {
        const auto port = process.port();
        check_origins(port);
        const auto initial = meowlive::tests::request(port, session_path);
        const auto initial_body = parse_object(initial.body);
        require(initial.status == 200 && initial.header("Cache-Control") == "no-store" &&
                    initial_body["enabled"].isBool() && initial_body["enabled"].asBool() &&
                    initial_body["authenticated"].isBool() &&
                    !initial_body["authenticated"].asBool(),
                "Unexpected state before administrator login");
        for (const auto credential : {std::string_view{}, meowlive::tests::admin_credential,
                                      meowlive::tests::device_credential})
        {
            expect_error(meowlive::tests::request(port, status_path, "GET", credential), 401,
                         "authentication_required");
        }
        for (const auto credential : {std::string_view{}, meowlive::tests::admin_credential})
        {
            const WebSocket denied(port, control_path, credential);
            expect_error(denied.response(), 401, "authentication_required");
        }
        require(meowlive::tests::request(port, health_path, "HEAD").status == 401,
                "Anonymous HEAD bypassed administrator authorization");
        expect_error(meowlive::tests::request(port, status_path, "POST"), 401,
                     "authentication_required");

        const auto login_body =
            std::format("{{\"token\":\"{}\"}}", meowlive::tests::admin_credential);
        check_login_body(port, login_body);
        auto token = login(port, login_body);
        require(meowlive::tests::request(port, status_path, "GET", token).status == 200,
                "Administrator session did not authorize a status request");
        const auto session = meowlive::tests::request(port, session_path, "GET", token);
        require(session.status == 200 && parse_object(session.body)["authenticated"].asBool(),
                "Administrator session did not authenticate");
        require(meowlive::tests::request(port, health_path, "HEAD", token).status == 200,
                "Administrator session did not authorize HEAD");
        const auto unsupported = meowlive::tests::request(port, status_path, "POST", token);
        require(unsupported.status == 405 && unsupported.header("Allow") == "GET,HEAD",
                "Unexpected response for an unsupported HTTP method");
        {
            const WebSocket denied(port, control_path, token);
            expect_error(denied.response(), 401, "authentication_required");
        }
        {
            WebSocket control(port, control_path, meowlive::tests::device_credential,
                              meowlive::tests::allowed_origin);
            const auto location = pair_path(hello(control, fixture));
            {
                const WebSocket denied(port, location, token);
                expect_error(denied.response(), 401, "authentication_required");
            }
            const WebSocket audio(port, location, meowlive::tests::device_credential,
                                  meowlive::tests::allowed_origin);
            require(audio.response().status == 101, "Authenticated audio upgrade failed");
            wait_health(process, true, fixture);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;)
        {
            const auto status = meowlive::tests::request(port, status_path, "GET", token);
            if (status.status == 401)
            {
                break;
            }
            require(std::chrono::steady_clock::now() < deadline,
                    "Expired administrator session remained valid");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        token = login(port, login_body);
        require(meowlive::tests::request(port, session_path, "DELETE", token).status == 204,
                "Administrator logout failed");
        expect_error(meowlive::tests::request(port, status_path, "GET", token), 401,
                     "authentication_required");
        for (unsigned attempt = 0; attempt < 5; ++attempt)
        {
            expect_error(meowlive::tests::request(port, session_path, "POST", {},
                                                  R"({"token":"incorrect-fixture"})"),
                         401, "authentication_required");
        }
        expect_error(meowlive::tests::request(port, session_path, "POST", {}, login_body), 429,
                     "login_rate_limited");
    }

    struct Options
    {
        std::filesystem::path executable;
        std::filesystem::path fixture{"tests/contracts/fixtures/protocol-v3.json"};
        bool rust{};
        bool help{};
    };

    template <typename Char> Options parse_arguments(int argc, Char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::filesystem::path argument(argv[index]);
            if (argument == "--help" || argument == "-h")
            {
                options.help = true;
                return options;
            }
            if (argument == "--rust")
            {
                options.rust = true;
            }
            else if (argument == "--fixture" && index + 1 < argc)
            {
                options.fixture = argv[++index];
            }
            else
            {
                require(!argument.native().starts_with(std::filesystem::path("-").native()) &&
                            !argument.empty() && options.executable.empty(),
                        "Expected --rust, --fixture PATH or a server executable");
                options.executable = argument;
            }
        }
        if (options.executable.empty())
        {
            require(!options.rust, "--rust requires a server executable");
#if defined(_WIN32)
            constexpr auto executable_name = "meowlive-server.exe";
#else
            constexpr auto executable_name = "meowlive-server";
#endif
            options.executable =
                std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path() /
                executable_name;
        }
        return options;
    }

    void run(const Options& options)
    {
        const auto fixture = read_fixture(options.fixture);
        const auto executable = std::filesystem::canonical(options.executable);
        Hello previous;
        {
            const ServerProcess process(executable, options.rust);
            wait_health(process, false, fixture);
            previous = check_bridge(process, fixture);
        }
        {
            const ServerProcess process(executable, options.rust);
            wait_health(process, false, fixture);
            check_restart(process, previous, fixture);
        }
        std::println("Protocol v3 handshake, timeout, conflict, receipts and reconnect passed.");
        {
            const ServerProcess process(executable, options.rust, true);
            wait_health(process, false, fixture);
            check_auth(process, fixture);
        }
        std::println("Administrator sessions, device credentials and origin passed.");
        if (!options.rust)
        {
            meowlive::tests::test_startup_configuration(executable);
        }
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        const auto options = parse_arguments(argc, argv);
        if (options.help)
        {
            std::print("{}", help);
        }
        else
        {
            run(options);
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Transport integration failed: {}", error.what());
        return 1;
    }
}
