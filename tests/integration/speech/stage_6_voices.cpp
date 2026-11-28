#include <json/json.h>

#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.control;
import meowlive.protocol.resources;
import meowlive.protocol.json;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    constexpr std::string_view legacy_id = "fixture-voice";
    constexpr std::string_view boundary = "meowlive-voice-fixture-7db4e2";
    constexpr std::string_view metadata =
        R"({"name":"Imported voice","language":"en","reference_text":"Test voice reference"})";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    ::Json::Value response_body(const meowlive::tests::HttpResponse& response, int status)
    {
        require(response.status == status, std::format("Expected {}, got HTTP {}: {}", status,
                                                       response.status, response.body));
        const auto body = meowlive::protocol::json::parse_object(response.body);
        require(body.has_value(), "Expected a JSON object");
        return *body;
    }

    std::string wav(bool silent)
    {
        constexpr std::uint32_t rate = 48000;
        constexpr std::uint32_t size = rate * 6 * 4;
        std::string result("RIFF");
        const auto append = [&](std::uint32_t value, unsigned width)
        {
            for (unsigned byte = 0; byte < width; ++byte)
            {
                result.push_back(static_cast<char>(value & 0xff));
                value >>= 8;
            }
        };
        append(36 + size, 4);
        result += "WAVEfmt ";
        append(16, 4);
        append(1, 2);
        append(2, 2);
        append(rate, 4);
        append(rate * 4, 4);
        append(4, 2);
        append(16, 2);
        result += "data";
        append(size, 4);
        result.resize(result.size() + size, '\0');
        if (!silent)
        {
            result[44] = '\1';
        }
        return result;
    }

    struct TestServer
    {
        TestServer(const std::filesystem::path& executable, const std::filesystem::path& root,
                   std::string_view log)
        {
            meowlive::tests::process::write_file(
                root / "voices.toml",
                std::format("[server]\nlisten_address = \"127.0.0.1:{}\"\n", port));
            child.start(executable, root, "voices.toml", true, std::filesystem::path(log));
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
            throw std::runtime_error("Voice test server did not start");
        }

        ::Json::Value snapshot() const
        {
            return response_body(meowlive::tests::request(port, "/api/resources"), 200);
        }

        ::Json::Value post(std::string_view path, int status,
                           std::optional<std::string_view> body = {}) const
        {
            return response_body(meowlive::tests::request(port, path, "POST", {}, body), status);
        }

        ::Json::Value select(std::string_view id, int status = 200) const
        {
            return post("/api/voices/select", status, std::format(R"({{"id":"{}"}})", id));
        }

        ::Json::Value remove(std::string_view id, int status = 200) const
        {
            return post("/api/voices/delete", status, std::format(R"({{"id":"{}"}})", id));
        }

        ::Json::Value upload(std::string_view audio, int status = 200) const
        {
            auto body = std::format(
                "--{}\r\nContent-Disposition: form-data; name=\"metadata\"\r\n\r\n{}\r\n"
                "--{}\r\nContent-Disposition: form-data; name=\"audio\"; "
                "filename=\"fixture.wav\"\r\n"
                "Content-Type: audio/wav\r\n\r\n",
                boundary, metadata, boundary);
            body += audio;
            body += std::format("\r\n--{}--\r\n", boundary);
            const auto type = std::format("multipart/form-data; boundary={}", boundary);
            return response_body(
                meowlive::tests::request(port, "/api/voices", "POST", {}, body, {}, type), status);
        }

        const std::uint16_t port = meowlive::tests::process::available_loopback_port();
        meowlive::tests::process::ChildProcess child;
    };

    std::string pair(meowlive::tests::WebSocket& control)
    {
        require(control.response().status == 101, "Control socket did not upgrade");
        control.send(1, meowlive::protocol::encode_client_message(
                            meowlive::protocol::ClientHello{meowlive::protocol::version}));
        const auto message = meowlive::protocol::decode_server_command(control.wait_frame(1));
        require(message && std::holds_alternative<meowlive::protocol::ServerHello>(*message),
                "Control socket did not return hello");
        const auto& hello = std::get<meowlive::protocol::ServerHello>(*message);
        return "/ws/audio?session_id=" + hello.session_id + "&bridge_id=" + hello.bridge_id;
    }

    template <typename Reply>
    ::Json::Value character_operation(const TestServer& server, meowlive::tests::WebSocket& control,
                                      std::string_view route, const std::string& body, Reply reply)
    {
        auto request = std::async(std::launch::async,
                                  [&]
                                  {
                                      return server.post(route, 200, body);
                                  });
        const auto command = meowlive::protocol::decode_server_command(control.wait_frame(1));
        require(command && std::holds_alternative<meowlive::protocol::ResourceCommand>(*command),
                "Character edit did not reach its paired execution endpoint");
        const auto& resource = std::get<meowlive::protocol::ResourceCommand>(*command);
        const auto response =
            meowlive::protocol::DesktopResourceResult::decode(reply(resource.operation.json()));
        require(response.has_value(), "Synthetic result mismatches resource protocol");
        control.send(
            1, meowlive::protocol::encode_client_message(
                   meowlive::protocol::ResourceResultMessage{resource.request_id, *response}));
        return request.get();
    }

    void check_characters(const TestServer& server, std::string_view voice,
                          const std::filesystem::path& catalog)
    {
        const auto saved = server.post("/api/characters/save", 200,
                                       std::format(R"({{
            "name":"Fixture character","model_id":"test-model","voice_id":"{}",
            "mouth_parameter":"MeowMouthOpen","mappings":[{{
                "intent":"test","hotkey_id":"test-action","fallback_hotkey_id":null,"validated":true
            }}]
        }})",
                                                   voice));
        const auto& profile = saved["characters"][saved["characters"].size() - 1];
        const auto id = profile["id"].asString();
        require(!id.empty() && !profile["mappings"][0u]["validated"].asBool(),
                "Character save accepted unchecked mapping validation");
        const auto selected = std::format(R"({{"id":"{}"}})", id);
        require(server.post("/api/characters/select", 400, selected)["code"] == "invalid_resource",
                "Selecting a model without its execution endpoint succeeded");

        meowlive::tests::WebSocket control(server.port, meowlive::protocol::control_path);
        meowlive::tests::WebSocket audio(server.port, pair(control));
        require(audio.response().status == 101, "Audio socket did not upgrade");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!response_body(meowlive::tests::request(server.port, "/api/health"),
                              200)["bridge_connected"]
                    .asBool())
        {
            require(std::chrono::steady_clock::now() < deadline, "Audio socket did not pair");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto before_selection = server.snapshot();
        const auto backup = catalog.parent_path() / "character-selection-backup";
        std::filesystem::rename(catalog, backup);
        std::filesystem::create_directory(catalog);
        auto failed_selection =
            std::async(std::launch::async,
                       [&]
                       {
                           return server.post("/api/characters/select", 500, selected);
                       });
        if (failed_selection.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        {
            const auto frame = meowlive::protocol::decode_server_command(control.wait_frame(1));
            require(frame && std::holds_alternative<meowlive::protocol::ResourceCommand>(*frame),
                    "Selection unexpectedly waited after the catalog write failed");
            const auto& command = std::get<meowlive::protocol::ResourceCommand>(*frame);
            ::Json::Value result(::Json::objectValue);
            result["type"] = "model_loaded";
            result["model_id"] = command.operation.json()["model_id"];
            control.send(1, meowlive::protocol::encode_client_message(
                                meowlive::protocol::ResourceResultMessage{
                                    command.request_id,
                                    *meowlive::protocol::DesktopResourceResult::decode(result)}));
            throw std::runtime_error("Selection loaded a model before its catalog was saved");
        }
        require(failed_selection.get()["code"] == "resource_save_failed" &&
                    server.snapshot() == before_selection,
                "A failed character save changed the active resources");
        std::filesystem::remove(catalog);
        std::filesystem::rename(backup, catalog);

        auto unconfirmed =
            std::async(std::launch::async,
                       [&]
                       {
                           return server.post("/api/characters/select", 400, selected);
                       });
        const auto answer_next_load = [&](std::string_view model, bool loaded)
        {
            const auto frame = meowlive::protocol::decode_server_command(control.wait_frame(1));
            require(frame && std::holds_alternative<meowlive::protocol::ResourceCommand>(*frame),
                    "Character recovery did not request a model");
            const auto& command = std::get<meowlive::protocol::ResourceCommand>(*frame);
            require(command.operation.json()["model_id"].asString() == model,
                    "Character recovery requested the wrong model");
            ::Json::Value response(::Json::objectValue);
            if (loaded)
            {
                response["type"] = "model_loaded";
                response["model_id"] = command.operation.json()["model_id"];
            }
            else
            {
                response["type"] = "error";
                response["code"] = "fixture_load_error";
                response["message"] = "Fixture failed to confirm the model";
            }
            control.send(1, meowlive::protocol::encode_client_message(
                                meowlive::protocol::ResourceResultMessage{
                                    command.request_id,
                                    *meowlive::protocol::DesktopResourceResult::decode(response)}));
        };
        answer_next_load("test-model", false);
        answer_next_load("fixture-model", true);
        require(unconfirmed.get()["code"] == "invalid_resource" &&
                    server.snapshot() == before_selection,
                "A rejected model switch did not restore the previous resources");

        const auto active = character_operation(
            server, control, "/api/characters/select", selected,
            [](const ::Json::Value& command)
            {
                require(command["type"] == "load_model" && command["model_id"] == "test-model" &&
                            command["mouth_parameter"] == "MeowMouthOpen",
                        "Selection lost its model or mouth parameter");
                ::Json::Value result(::Json::objectValue);
                result["type"] = "model_loaded";
                result["model_id"] = command["model_id"];
                return result;
            });
        require(active["active_character_id"] == id &&
                    active["active_voice_id"].asString() == voice,
                "Accepted model did not activate character and its voice");
        const auto previewed =
            character_operation(server, control, "/api/characters/preview",
                                std::format(R"({{"character_id":"{}","intent":"test"}})", id),
                                [](const ::Json::Value& command)
                                {
                                    require(command["type"] == "trigger_hotkey" &&
                                                command["hotkey_id"] == "test-action",
                                            "Preview lost the saved mapping");
                                    ::Json::Value result(::Json::objectValue);
                                    result["type"] = "hotkey_triggered";
                                    result["hotkey_id"] = command["hotkey_id"];
                                    return result;
                                });
        require(
            previewed["characters"][previewed["characters"].size() - 1]["mappings"][0u]["validated"]
                .asBool(),
            "Confirmed preview did not persist validation");
        require(
            server.post("/api/characters/delete", 200, selected)["active_character_id"].isNull(),
            "Deleting a character kept its active ID");
    }

    void check_busy(const TestServer& server, std::string_view voice)
    {
        meowlive::tests::WebSocket control(server.port, meowlive::protocol::control_path);
        meowlive::tests::WebSocket audio(server.port, pair(control));
        require(audio.response().status == 101, "Audio socket did not upgrade");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!response_body(meowlive::tests::request(server.port, "/api/health"),
                              200)["bridge_connected"]
                    .asBool())
        {
            require(std::chrono::steady_clock::now() < deadline, "Audio socket did not pair");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        server.post("/api/speech/preview", 202);
        const auto message = meowlive::protocol::decode_server_command(control.wait_frame(1));
        require(message && std::holds_alternative<meowlive::protocol::ServerSpeak>(*message),
                "Preview did not reach playback");
        require(server.select(voice, 409)["code"] == "resource_busy" &&
                    server.remove(voice, 409)["code"] == "resource_busy",
                "An in-flight playback permitted voice edits");
        server.post("/api/stop", 200);
    }

    void verify(const std::filesystem::path& executable)
    {
        const meowlive::tests::process::TestDirectory directory;
        const auto resources = directory.path / "data" / "resources";
        const auto references = resources / "references";
        const auto catalog = resources / "catalog.json";
        std::filesystem::create_directories(references);
        const auto valid_wav = wav(false);
        const auto legacy_reference = references / "fixture-voice.wav";
        meowlive::tests::process::write_file(legacy_reference, valid_wav);
        meowlive::tests::process::write_file(catalog, R"({
          "schema_version":1,"active_voice_id":"fixture-voice","active_character_id":"fixture-character",
          "voices":[{"id":"fixture-voice","reference":"fixture-voice","name":"Legacy voice",
            "language":"en","reference_text":"Legacy reference","duration_ms":6000,"sample_rate":48000,"channels":2}],
          "characters":[{"id":"fixture-character","name":"Legacy character","model_id":"fixture-model",
            "voice_id":"fixture-voice","mouth_parameter":"MeowMouthOpen","mappings":[]}]
        })");

        std::string id;
        {
            const TestServer server(executable, directory.path, "first.log");
            const auto original = server.snapshot();
            require(original["active_voice_id"].asString() == legacy_id &&
                        original["voices"][0u]["available"] == true &&
                        !original["voices"][0u].isMember("reference"),
                    "Legacy catalog did not restore or exposed the private WAV reference");

            require(server.upload("broken", 400)["code"] == "invalid_resource" &&
                        server.upload(wav(true), 400)["code"] == "invalid_resource",
                    "Invalid or silent WAV was imported");
            require(server.snapshot() == original, "Invalid upload changed the resource catalog");

            const auto rollback = resources / "catalog-backup";
            std::filesystem::rename(catalog, rollback);
            std::filesystem::create_directory(catalog);
            require(server.upload(valid_wav, 500)["code"] == "resource_failed" &&
                        server.snapshot() == original &&
                        std::distance(std::filesystem::directory_iterator(references),
                                      std::filesystem::directory_iterator{}) == 1,
                    "Failed catalog update changed state or left a WAV file");
            std::filesystem::remove(catalog);
            std::filesystem::rename(rollback, catalog);

            const auto imported = server.upload(valid_wav);
            const auto& voice = imported["voices"][1u];
            id = voice["id"].asString();
            require(!id.empty() && voice["available"] == true && voice["duration_ms"] == 6000 &&
                        voice["sample_rate"] == 48000 && voice["channels"] == 2 &&
                        imported["active_voice_id"].asString() == legacy_id &&
                        std::filesystem::file_size(references / (id + ".wav")) == valid_wav.size(),
                    "Imported voice data or existing active voice was lost");
            check_busy(server, id);
            require(server.select(id)["active_voice_id"] == id,
                    "Selecting the imported voice did not change active voice");
        }
        {
            const TestServer server(executable, directory.path, "restart.log");
            require(server.snapshot()["active_voice_id"] == id, "Restart lost active voice");
            check_characters(server, id, catalog);
            const auto after_legacy = server.remove(legacy_id);
            require(after_legacy["voices"].size() == 1 &&
                        after_legacy["characters"][0u]["voice_id"] == "" &&
                        after_legacy["active_character_id"].isNull() &&
                        !std::filesystem::exists(legacy_reference),
                    "Deleting a legacy voice did not update its character and remove the WAV");
            const auto after_active = server.remove(id);
            require(after_active["active_voice_id"] == "" && after_active["voices"].empty() &&
                        !std::filesystem::exists(references / (id + ".wav")),
                    "Deleting the active voice did not clear it");
        }
        {
            const TestServer server(executable, directory.path, "deleted.log");
            require(server.snapshot()["voices"].empty() &&
                        server.snapshot()["active_voice_id"] == "",
                    "A deleted voice returned after restart");
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
        require(argc <= 2, "meowlive-voice-integration [SERVER]");
        const auto executable = argc == 2 ? std::filesystem::path(argv[1])
                                          : std::filesystem::absolute(argv[0]).parent_path() /
#if defined(_WIN32)
                                                "meowlive-server.exe";
#else
                                                "meowlive-server";
#endif
        verify(executable);
        std::println(
            "Voice import, validation, playback exclusion, rollback and persistence passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Voice integration failed: {}", error.what());
        return 1;
    }
}
