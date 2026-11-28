module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.tests.stage_2_startup;

import meowlive.standard;

import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    constexpr std::string_view private_marker = "stage-2-fixture-private-marker";
    constexpr std::string_view authenticated =
        "[auth]\nenabled = true\nadmin_token_env = \"\"\ndevice_token_env = \"\"\n"
        "admin_token_file = \"admin.token\"\ndevice_token_file = \"device.token\"\n";

    std::string read_file(const std::filesystem::path& path)
    {
        const std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Cannot read the startup fixture output");
        }
        return {std::istreambuf_iterator<char>(input.rdbuf()), std::istreambuf_iterator<char>()};
    }

    void expect_startup_failure(const std::filesystem::path& executable,
                                const std::filesystem::path& directory,
                                std::string_view expected_field)
    {
        meowlive::tests::process::ChildProcess child;
        child.start(executable, directory, "config/launch.toml", true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (const auto status = child.exit_code())
            {
                const auto log = read_file(directory / "server.log");
                if (*status != 1 || !log.contains(expected_field) || log.contains(private_marker) ||
                    !log.contains("operation=server startup exception=std::runtime_error") ||
                    log.contains("MeowLive2D protocol"))
                {
                    throw std::runtime_error(
                        std::format("Expected redacted startup failure for {}", expected_field));
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error(
            std::format("Server kept running with invalid {}", expected_field));
    }
}

export namespace meowlive::tests
{
    void test_startup_configuration(const std::filesystem::path& executable)
    {
        {
            const meowlive::tests::process::TestDirectory directory;
            const auto configuration = directory.path / "config";
            std::filesystem::create_directory(configuration);
            meowlive::tests::process::write_file(
                configuration / "launch.toml",
                std::format("[server]\nqueue_capacity = \"{}\"\n", private_marker));
            expect_startup_failure(executable, directory.path, "server.queue_capacity");
        }
        for (const std::string_view expected_field :
             {std::string_view("auth.admin_token_file"), std::string_view("auth.device_token_file"),
              std::string_view("[auth]")})
        {
            const meowlive::tests::process::TestDirectory directory;
            const auto configuration = directory.path / "config";
            std::filesystem::create_directory(configuration);
            meowlive::tests::process::write_file(configuration / "launch.toml", authenticated);
            if (expected_field != "auth.admin_token_file")
            {
                meowlive::tests::process::write_file(configuration / "admin.token",
                                                     meowlive::tests::admin_credential);
                meowlive::tests::process::write_file(configuration / "device.token",
                                                     expected_field == "[auth]"
                                                         ? meowlive::tests::admin_credential
                                                         : private_marker);
            }
            expect_startup_failure(executable, directory.path, expected_field);
        }
        {
            const meowlive::tests::process::TestDirectory directory;
            const auto configuration = directory.path / "config";
            std::filesystem::create_directories(configuration / "local");
            meowlive::tests::process::write_file(configuration / "launch.toml", "[server]\n");
            meowlive::tests::process::write_file(
                configuration / "local" / "launch-agent.json",
                std::format(R"({{"schema":7,"private":"{}"}})", private_marker));
            expect_startup_failure(executable, directory.path, "-agent.json: schema");
            const auto diagnostic =
                read_file(directory.path / "data/logs/meowlive-server.exceptions.log");
            if (!diagnostic.contains("-agent.json: schema") || diagnostic.contains(private_marker))
            {
                throw std::runtime_error("Expected redacted startup failure in the exception log");
            }
        }
        {
            const meowlive::tests::process::TestDirectory directory;
            const auto configuration = directory.path / "config";
            std::filesystem::create_directories(configuration / "local");
            const auto profile_path = configuration / "local" / "launch-agent.json";
            constexpr std::string_view legacy_profile =
                R"({"schema":1,"settings":{"persona":"fixture","topic":"","proactive_enabled":false,"cooldown_ms":30000}})";
            meowlive::tests::process::write_file(profile_path, legacy_profile);
            const auto port = meowlive::tests::process::available_loopback_port();
            meowlive::tests::process::write_file(
                configuration / "launch.toml",
                std::format("[server]\nlisten_address = \"127.0.0.1:{}\"\nqueue_capacity = 1\n",
                            port));
            meowlive::tests::process::ChildProcess child;
            child.start(executable, directory.path, "config/launch.toml", true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < deadline)
            {
                child.assert_running();
                try
                {
                    if (meowlive::tests::request(port, "/api/health").status == 200)
                    {
                        if (read_file(profile_path) != legacy_profile)
                        {
                            throw std::runtime_error("Startup modified the legacy profile");
                        }
                        return;
                    }
                }
                catch (const std::system_error&)
                {
                    // The listener may still be starting.
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            throw std::runtime_error("Server did not start with a legacy profile");
        }
    }
}
