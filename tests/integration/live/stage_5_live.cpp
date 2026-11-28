#include <json/json.h>

#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.json;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    namespace tests = meowlive::tests;
    namespace wire = meowlive::protocol;

    constexpr std::string_view live_status = "/api/live";
    constexpr std::string_view live_settings = "/api/live/settings";
    constexpr std::string_view live_connect = "/api/live/connect";
    constexpr std::string_view live_disconnect = "/api/live/disconnect";
    constexpr auto fixture_id = "live-fixture-id";
    constexpr auto fixture_secret = "live-fixture-signing-placeholder";
    constexpr auto fixture_identity = "live-fixture-anchor-placeholder";
    constexpr auto app_id = "9007199254740993";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    ::Json::Value response_body(std::string_view path, const tests::HttpResponse& response,
                                int status)
    {
        require(response.status == status,
                std::format("{} returned HTTP {}, expected {}", path, response.status, status));
        require(!response.body.contains(fixture_id) && !response.body.contains(fixture_secret) &&
                    !response.body.contains(fixture_identity),
                "Live response disclosed credentials");
        const auto value = wire::json::parse_object(response.body);
        require(value.has_value(), "Expected a JSON object");
        return *value;
    }

    struct TestServer
    {
        TestServer(const std::filesystem::path& executable, const std::filesystem::path& root,
                   std::string_view log)
        {
            tests::process::write_file(root / "admin.token", tests::admin_credential);
            tests::process::write_file(root / "device.token", tests::device_credential);
            tests::process::write_file(root / "broadcast.toml", std::format(R"([server]
listen_address = "127.0.0.1:{}"
[auth]
enabled = true
admin_token_env = ""
device_token_env = ""
admin_token_file = "admin.token"
device_token_file = "device.token"
[live]
enabled = false
access_key_id_env = "MEOWLIVE_TEST_LIVE_UNUSED_ID"
access_key_secret_env = "MEOWLIVE_TEST_LIVE_UNUSED_SECRET"
identity_code_env = "MEOWLIVE_TEST_LIVE_UNUSED_IDENTITY"
)",
                                                                            port));
            child.start(executable, root, "broadcast.toml", true, std::filesystem::path(log));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (std::chrono::steady_clock::now() < deadline)
            {
                child.assert_running();
                try
                {
                    if (tests::request(port, "/api/health").status == 200)
                    {
                        authenticate();
                        return;
                    }
                }
                catch (const std::system_error&)
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            throw std::runtime_error("Live settings test server did not start");
        }

        ::Json::Value get(std::string_view path, int status = 200) const
        {
            return response_body(path, tests::request(port, path, "GET", token), status);
        }

        ::Json::Value post(std::string_view path, int status = 200,
                           std::optional<std::string_view> body = {}) const
        {
            return response_body(path, tests::request(port, path, "POST", token, body), status);
        }

        ::Json::Value save(const ::Json::Value& settings, int status = 200) const
        {
            return post(live_settings, status, wire::json::serialize(settings));
        }

        void authenticate()
        {
            constexpr std::string_view path = "/api/admin/session";
            const auto payload = std::format(R"({{"token":"{}"}})", tests::admin_credential);
            token =
                response_body(path, tests::request(port, path, "POST", {}, payload), 200)["token"]
                    .asString();
            require(!token.empty(), "Missing administrator session token");
        }

        const std::uint16_t port = tests::process::available_loopback_port();
        tests::process::ChildProcess child;
        std::string token;
    };

    ::Json::Value settings(bool enabled, const char* application)
    {
        ::Json::Value request(::Json::objectValue);
        request["enabled"] = enabled;
        request["app_id"] = application;
        return request;
    }

    bool configured_credentials(const ::Json::Value& snapshot, bool configured)
    {
        return snapshot["access_key_id_configured"] == configured &&
               snapshot["access_key_secret_configured"] == configured &&
               snapshot["identity_code_configured"] == configured;
    }

    void check_first_start(const TestServer& server, const std::filesystem::path& settings_file)
    {
        for (const auto path : {live_status, live_settings})
        {
            const auto anonymous = tests::request(server.port, path);
            require(anonymous.status == 401, "Anonymous request returned live settings or status");
        }
        for (const auto path : {live_connect, live_disconnect, live_settings})
        {
            const auto device = tests::request(server.port, path, "POST", tests::device_credential);
            require(device.status == 401, "Device credential authorized a live control request");
        }

        const auto initial = server.get(live_settings);
        require(initial["enabled"] == false && initial["app_id"] == "" &&
                    initial["storage_available"] == true && configured_credentials(initial, false),
                "Initial live settings are not disabled and empty");
        require(server.get(live_status)["phase"] == "disabled",
                "Initial live connection phase is not disabled");
        require(server.post(live_connect, 409)["code"] == "live_unconfigured",
                "Disabled live source accepted a connection");
        require(server.post(live_disconnect)["phase"] == "disabled",
                "Disconnect changed a disabled source");
        require(server.post(live_settings, 400, "{")["code"] == "invalid_live_settings",
                "Malformed live settings JSON had an unexpected response");
        require(server.save(settings(true, app_id), 400)["code"] == "invalid_live_settings",
                "Enabling live without credentials succeeded");

        auto complete = settings(true, app_id);
        complete["access_key_id"] = fixture_id;
        complete["access_key_secret"] = fixture_secret;
        complete["identity_code"] = fixture_identity;
        const auto saved = server.save(complete);
        require(saved["enabled"] == true && saved["app_id"] == app_id &&
                    configured_credentials(saved, true),
                "Complete credentials or full application ID were lost");
        const auto status = server.get(live_status);
        require(status["phase"] == "disconnected" && status["configured"] == true &&
                    status["room_id"].isNull(),
                "Saving settings opened a connection");
        require(server.save(settings(true, app_id)) == saved,
                "Saving with unchanged application ID discarded stored credentials");
        require(server.save(settings(true, "2"), 400)["code"] == "invalid_live_settings",
                "Changing application ID without replacement credentials succeeded");

        const auto backup = settings_file.parent_path() / "broadcast-live-backup.json";
        std::filesystem::rename(settings_file, backup);
        std::filesystem::create_directory(settings_file);
        const auto failed = server.save(settings(false, app_id), 500);
        require(failed["code"] == "live_settings_save_failed" && server.get(live_settings) == saved,
                "Failed settings write changed live settings");
        std::filesystem::remove(settings_file);
        std::filesystem::rename(backup, settings_file);
    }

    void verify(const std::filesystem::path& executable)
    {
        const tests::process::TestDirectory directory;
        const auto settings_file = directory.path / "local" / "broadcast-live.json";
        {
            const TestServer server(executable, directory.path, "first.log");
            check_first_start(server, settings_file);
        }
        {
            const TestServer server(executable, directory.path, "restart.log");
            const auto snapshot = server.get(live_settings);
            require(snapshot["enabled"] == true && snapshot["app_id"] == app_id &&
                        configured_credentials(snapshot, true) &&
                        server.get(live_status)["phase"] == "disconnected",
                    "Restart lost the saved settings or connected automatically");

            auto clear = settings(true, app_id);
            clear["clear_credentials"] = true;
            require(server.save(clear, 400)["code"] == "invalid_live_settings",
                    "Credential clearing was allowed while live remained enabled");
            clear["enabled"] = false;
            const auto cleared = server.save(clear);
            require(cleared["enabled"] == false && configured_credentials(cleared, false),
                    "Credential clearing did not disable live and remove all credentials");
        }
        {
            const TestServer server(executable, directory.path, "cleared.log");
            const auto snapshot = server.get(live_settings);
            require(snapshot["enabled"] == false && configured_credentials(snapshot, false),
                    "Restart recovered cleared credentials");
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
        require(argc <= 2, "meowlive-live-integration [SERVER]");
        const auto executable = argc == 2 ? std::filesystem::path(argv[1])
                                          : std::filesystem::absolute(argv[0]).parent_path() /
#if defined(_WIN32)
                                                "meowlive-server.exe";
#else
                                                "meowlive-server";
#endif
        verify(executable);
        std::println("Live HTTP authorization, redaction, settings validation and restart passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Live integration failed: {}", error.what());
        return 1;
    }
}
