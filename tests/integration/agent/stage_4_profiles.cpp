#include <json/json.h>

#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.json;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    constexpr std::string_view personas = "/api/agent/personas";
    constexpr std::string_view models = "/api/llm/settings";
    constexpr std::string_view runtime = "/api/agent/runtime";
    constexpr std::string_view fixture_key = "test-profile-placeholder";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    ::Json::Value config(std::string_view model = "stage4-fixture")
    {
        auto settings = *meowlive::protocol::json::parse_object(R"({
            "provider":"custom","api_format":"openai_chat","base_url":"http://127.0.0.1:19001/v1",
            "model":"stage4-fixture","mode":"cloud","timeout_seconds":30,"max_tokens":1024,
            "json_mode":true,"reasoning_effort":"default"
        })");
        settings["model"] = std::string(model);
        return settings;
    }

    ::Json::Value draft(std::string_view model)
    {
        ::Json::Value body(::Json::objectValue);
        body["settings"] = config(model);
        body["clear_api_key"] = false;
        return body;
    }

    ::Json::Value object(std::string_view body)
    {
        auto value = meowlive::protocol::json::parse_object(body);
        require(value.has_value(), "Expected fixture JSON object");
        return std::move(*value);
    }

    ::Json::Value read_file(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        require(static_cast<bool>(stream), "Expected persisted settings file");
        ::Json::Value value;
        stream >> value;
        return value;
    }

    struct TestServer
    {
        TestServer(const std::filesystem::path& executable, const std::filesystem::path& directory,
                   std::string_view log)
        {
            meowlive::tests::process::write_file(
                directory / "custom.toml",
                "[server]\nlisten_address = \"127.0.0.1:" + std::to_string(port) + "\"\n");
            child.start(executable, directory, "custom.toml", true, std::filesystem::path(log));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (std::chrono::steady_clock::now() < deadline)
            {
                child.assert_running();
                try
                {
                    if (meowlive::tests::request(port, "/api/health").status == 200)
                    {
                        return;
                    }
                }
                catch (const std::system_error&)
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            throw std::runtime_error("Settings test server did not start");
        }

        ::Json::Value request(std::string_view path, std::optional<::Json::Value> input = {},
                              int status = 200) const
        {
            const auto body =
                input ? std::optional(meowlive::protocol::json::serialize(*input)) : std::nullopt;
            const auto response =
                meowlive::tests::request(port, path, input ? "POST" : "GET", {}, body);
            require(response.status == status,
                    std::string(path) + " gave HTTP " + std::to_string(response.status));
            require(!response.body.contains(fixture_key), "Settings response exposed fixture key");
            return object(response.body);
        }

        const std::uint16_t port = meowlive::tests::process::available_loopback_port();
        meowlive::tests::process::ChildProcess child;
    };

    void verify_runtime_settings(const TestServer& server, const std::filesystem::path& data)
    {
        require(server.request(runtime, object(R"({"settings":{"max_tool_rounds":0}})"),
                               400)["code"] == "invalid_agent_runtime",
                "Invalid runtime settings must return a validation error");

        const auto path = data / "resources" / "agent-runtime" / "settings.json";
        std::filesystem::create_directories(path);
        const auto update = object(R"({"settings":{"max_tool_rounds":3}})");
        const auto failure = server.request(runtime, update, 500);
        require(failure["code"] == "agent_runtime_save_failed" &&
                    server.request(runtime)["settings"]["max_tool_rounds"] == 2,
                "Failed runtime write must report a storage error and retain the old setting");
        std::filesystem::remove(path);
        require(server.request(runtime, update)["settings"]["max_tool_rounds"] == 3 &&
                    std::filesystem::is_regular_file(path),
                "Runtime settings write must succeed after removing the obstruction");
    }

    void verify(const std::filesystem::path& executable)
    {
        const meowlive::tests::process::TestDirectory directory;
        const auto local = directory.path / "local";
        const auto agent_path = local / "custom-agent.json";
        const auto model_path = local / "custom-llm.json";
        std::filesystem::create_directories(local);
        auto initial_agent = object(
            R"({"persona":"Legacy persona","topic":"","cooldown_ms":30000,"proactive_enabled":false})");
        auto original_agent = object(R"({"schema":1})");
        original_agent["settings"] = initial_agent;
        auto original_model = object(R"({"schema":1})");
        original_model["config"] = config();
        original_model["api_key"] = std::string(fixture_key);
        meowlive::tests::process::write_file(agent_path,
                                             meowlive::protocol::json::serialize(original_agent));
        meowlive::tests::process::write_file(model_path,
                                             meowlive::protocol::json::serialize(original_model));

        std::string selected;
        std::string selected_model;
        {
            const TestServer server(executable, directory.path, "first.log");
            verify_runtime_settings(server, directory.path / "data");
            const auto migrated = server.request(personas);
            require(migrated["selected_profile_id"] == "legacy" &&
                        migrated["profiles"][0u]["persona"] == "Legacy persona",
                    "Agent schema 1 migration did not select legacy persona");
            const auto legacy_model = server.request(models);
            require(legacy_model["selected_profile_id"] == "legacy" &&
                        legacy_model["key_configured"].asBool() &&
                        legacy_model["active_model"] == "stage4-fixture",
                    "LLM schema 1 migration missed selected model or key state");

            const auto created = server.request(
                personas,
                object(R"({"name":"Profile B","persona":"【人物卡 v1】\n身份：测试主播"})"));
            selected = created["selected_profile_id"].asString();
            require(!selected.empty() && selected != "legacy", "New persona was not selected");
            const auto stale = server.request(
                "/api/agent/personas/update",
                object(R"({"id":"legacy","name":"Stale Editor","persona":"No overwrite"})"), 409);
            require(stale["code"] == "persona_profile_changed", "Stale persona edit was accepted");
            initial_agent["cooldown_ms"] = 60'000;
            server.request("/api/agent/settings", initial_agent);
            const auto committed = read_file(agent_path);
            require(committed["schema"].asInt() == 2 &&
                        committed["selected_profile_id"] == selected &&
                        committed["settings"]["cooldown_ms"].asInt() == 60'000 &&
                        committed["settings"]["persona"] == created["profiles"][1u]["persona"],
                    "General settings overwrote active persona or lost cooldown");

            const auto backup = local / "settings-backup";
            std::filesystem::rename(agent_path, backup);
            std::filesystem::create_directory(agent_path);
            auto rename = object(R"({"name":"Storage failure"})");
            rename["id"] = selected;
            const auto failure = server.request("/api/agent/personas/rename", rename, 500);
            require(failure["code"] == "persona_profiles_unavailable" &&
                        server.request(personas)["profiles"][1u]["name"] == "Profile B",
                    "Failed writes changed the visible persona");
            std::filesystem::remove(agent_path);
            std::filesystem::rename(backup, agent_path);

            auto new_model = draft("stage4-second");
            new_model["name"] = "Model B";
            new_model["settings"]["base_url"] = "http://127.0.0.1:19002/v1";
            const auto second = server.request("/api/llm/profiles", new_model);
            selected_model = second["selected_profile_id"].asString();
            require(!selected_model.empty() && selected_model != "legacy" &&
                        !second["key_configured"].asBool() && second["restart_required"].asBool(),
                    "Changed destination inherited credentials or restarted model early");
            auto stale_model = draft("would-overwrite");
            stale_model["id"] = "legacy";
            require(server.request(models, stale_model, 409)["code"] == "llm_profile_changed",
                    "Stale model edit was accepted");
            require(read_file(model_path)["schema"].asInt() == 2,
                    "LLM edits did not persist schema 2");
        }
        {
            const TestServer server(executable, directory.path, "restart.log");
            require(server.request(personas)["selected_profile_id"] == selected,
                    "Persona selection did not survive restart");
            const auto state = server.request(models);
            require(state["selected_profile_id"] == selected_model &&
                        state["active_model"] == "stage4-second" &&
                        !state["restart_required"].asBool(),
                    "LLM selection did not become active on restart");
        }
    }

    void verify_observability_recovery(const std::filesystem::path& executable)
    {
        const meowlive::tests::process::TestDirectory directory;
        const auto runtime = directory.path / "data" / "resources" / "agent-runtime";
        std::filesystem::create_directories(runtime);
        meowlive::tests::process::write_file(
            runtime / "active-trace.jsonl",
            "{\"id\":\"42\",\"state\":\"speaking\",\"created_at_ms\":1700000000000,"
            "\"event_ids\":[\"fixture-event\"],\"persona\":\"test-profile-placeholder\"}\n");
        meowlive::tests::process::write_file(
            runtime / "turns.jsonl",
            "{\"trace_id\":\"42\",\"provider\":\"test\",\"model\":\"mock\","
            "\"started_at_ms\":1700000000001,\"input_tokens\":6,\"output_tokens\":4,"
            "\"total_tokens\":10,\"api_key\":\"test-profile-placeholder\"}\n");
        for (const auto* log : {"recovery.log", "recovery-restart.log"})
        {
            const TestServer server(executable, directory.path, log);
            const auto trace = server.request("/api/agent/traces/42");
            require(trace["state"] == "unknown" && trace["event_ids"][0u] == "fixture-event",
                    "Interrupted trace was not restored as unknown");
            const auto listed = server.request("/api/agent/traces");
            require(listed["traces"].isArray() && listed["traces"].size() == 1,
                    "Interrupted trace was duplicated during recovery");
            const auto usage = server.request("/api/llm/usage");
            require(usage["total_tokens"].asUInt64() == 10,
                    "Previously recorded token usage was not restored");
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
        require(argc <= 2, "meowlive-profile-integration [SERVER]");
        const auto path = argc == 2 ? std::filesystem::path(argv[1])
                                    : std::filesystem::absolute(argv[0]).parent_path() /
#if defined(_WIN32)
                                          "meowlive-server.exe";
#else
                                          "meowlive-server";
#endif
        verify(path);
        verify_observability_recovery(path);
        std::println("Persona and LLM profile migration, stale edits, storage, restart and trace "
                     "recovery passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Profile integration failed: {}", error.what());
        return 1;
    }
}
