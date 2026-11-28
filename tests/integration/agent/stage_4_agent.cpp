#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <cstdio>
#if defined(_WIN32)
#include <cstdlib>
#endif
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.control;
import meowlive.protocol.json;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    namespace tests = meowlive::tests;
    namespace wire = meowlive::protocol;
    using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
    constexpr std::string_view audition_id = "00000000-0000-0000-0000-000000000017";
    constexpr std::string_view audition_text = "queue-isolation-audition";
#endif

    void require(bool passed, std::string_view message)
    {
        if (!passed)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    ::Json::Value body(const tests::HttpResponse& response, unsigned status)
    {
        require(response.status == status,
                std::format("Expected HTTP {}, got {}", status, response.status));
        const auto content = wire::json::parse_object(response.body);
        require(content.has_value(), "Expected a JSON response");
        return *content;
    }

    template <typename Probe>::Json::Value wait(Probe probe, std::string_view message)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(6);
        while (Clock::now() < deadline)
        {
            auto result = probe();
            if (!result.isNull())
            {
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        throw std::runtime_error(std::string(message));
    }

    std::string event(std::string_view id)
    {
        return std::format(
            R"({{"events":[{{"id":"{}","source":"simulator","viewer":"Viewer","kind":{{"type":"chat","text":"Hello from chat"}}}}]}})",
            id);
    }

    ::Json::Value find_event(const ::Json::Value& snapshot, std::string_view id)
    {
        for (const auto& row : snapshot["events"])
        {
            if (row["event"]["id"].asString() == id)
            {
                return row;
            }
        }
        return {};
    }

    template <typename Command> Command next_command(tests::WebSocket& control)
    {
        const auto decoded = wire::decode_server_command(control.wait_frame(1));
        require(decoded && std::holds_alternative<Command>(*decoded), "Unexpected control command");
        return std::get<Command>(*decoded);
    }

    void send_receipt(tests::WebSocket& control, const wire::ServerSpeak& command,
                      wire::ExecutionStatus status)
    {
        const auto data = wire::encode_client_message(
            wire::ExecutionReceipt{command.utterance_id, command.generation, status, {}});
        control.send(1, data);
    }

    void send_obs_status(tests::WebSocket& control)
    {
        const auto command = next_command<wire::ResourceCommand>(control);
        const auto& operation = command.operation.json();
        require(operation["type"] == "obs" && operation["operation"]["type"] == "status",
                "Expected a read-only OBS status request");
        const auto result = wire::json::parse_object(R"({"type":"obs","snapshot":{
            "connected":true,"recording":true,"current_scene":"Studio","scenes":["Studio"]}})");
        control.send(
            1, wire::encode_client_message(wire::ResourceResultMessage{
                   command.request_id, wire::DesktopResourceResult::decode(*result).value()}));
    }

    class HeldResponse
    {
    public:
        bool wait()
        {
            started_.release();
            return release_.try_acquire_for(std::chrono::seconds(5));
        }

        void during(const std::function<void()>& check)
        {
            const struct Release
            {
                std::binary_semaphore& gate;
                ~Release()
                {
                    gate.release();
                }
            } release{release_};
            require(started_.try_acquire_for(std::chrono::seconds(3)),
                    "Controlled backend did not receive the request");
            check();
        }

    private:
        std::counting_semaphore<> started_{0};
        std::binary_semaphore release_{0};
    };

    struct FakeModel
    {
        std::atomic<unsigned> calls{};
        std::atomic<bool> started_hold{};
        std::atomic<bool> saw_history{};
        std::atomic<bool> saw_obs_status{};
        HeldResponse model_listing;
#if defined(_WIN32)
        HeldResponse training_audition;
#endif
    };

    void install_fixture(unsigned short port, FakeModel& model, const std::filesystem::path& dir)
    {
        auto& app = drogon::app();
        app.addListener("127.0.0.1", port)
            .setUploadPath(drogon::utils::fromNativePath((dir / "fixture-uploads").native()))
            .setTermSignalHandler(
                []
                {
                })
            .setIntSignalHandler(
                []
                {
                });
        app.registerHandler("/v1/models",
                            [&model](const drogon::HttpRequestPtr&,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& send)
                            {
                                auto reply = drogon::HttpResponse::newHttpResponse();
                                if (model.model_listing.wait())
                                {
                                    reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                                    reply->setBody(R"({"data":[{"id":"held-model"}]})");
                                }
                                else
                                {
                                    reply->setStatusCode(drogon::k504GatewayTimeout);
                                }
                                send(reply);
                            },
                            {drogon::Get});
#if defined(_WIN32)
        for (const auto* path : {"/set_gpt_weights", "/set_sovits_weights"})
        {
            app.registerHandler(path,
                                [](const drogon::HttpRequestPtr&,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& send)
                                {
                                    send(drogon::HttpResponse::newHttpResponse());
                                },
                                {drogon::Get});
        }
#endif
        app.registerHandler(
            "/v1/chat/completions",
            [&model](const drogon::HttpRequestPtr& request,
                     std::function<void(const drogon::HttpResponsePtr&)>&& send)
            {
                ++model.calls;
                const auto input = wire::json::parse_object(request->body());
                auto response = ::Json::Value(::Json::objectValue);
                ::Json::Value choice(::Json::objectValue);
                choice["finish_reason"] = "stop";
                choice["message"]["role"] = "assistant";
                const auto original = request->body();
                std::string id = original.contains("cancel-chat") ? "cancel-chat"
                                 : original.contains("next-chat") ? "next-chat"
                                 : original.contains("bad-chat")  ? "missing-event"
                                                                  : "demo-chat-1";
                if (original.contains("next-chat") && original.contains("First successful reply"))
                {
                    model.saw_history = true;
                }
                if (!input)
                {
                    choice["message"]["content"] = "{}";
                }
                else if (id == "demo-chat-1" && !original.contains("tool-fixture"))
                {
                    choice["finish_reason"] = "tool_calls";
                    const auto add_tool = [&](const char* call_id, const char* name)
                    {
                        ::Json::Value tool(::Json::objectValue);
                        tool["id"] = call_id;
                        tool["type"] = "function";
                        tool["function"]["name"] = name;
                        tool["function"]["arguments"] = "{}";
                        choice["message"]["tool_calls"].append(std::move(tool));
                    };
                    add_tool("tool-fixture", "get_current_time");
                    add_tool("obs-fixture", "get_obs_status");
                }
                else
                {
                    for (const auto& message : (*input)["messages"])
                    {
                        if (message["role"] == "tool" && message["tool_call_id"] == "obs-fixture")
                        {
                            const auto observed =
                                wire::json::parse_object(message["content"].asString());
                            model.saw_obs_status = observed && (*observed)["connected"] == true &&
                                                   (*observed)["recording"] == true &&
                                                   (*observed)["current_scene"] == "Studio" &&
                                                   (*observed)["observed_at_ms"].isUInt64();
                        }
                    }
                    ::Json::Value decision(::Json::objectValue);
                    decision["reply_to"].append(id);
                    decision["text"] = "First successful reply.";
                    decision["topic"] = ::Json::Value{};
                    choice["message"]["content"] = wire::json::serialize(decision);
                }
                response["choices"].append(std::move(choice));
                response["usage"]["prompt_tokens"] = 20;
                response["usage"]["completion_tokens"] = 8;
                response["usage"]["total_tokens"] = 28;
                auto output = drogon::HttpResponse::newHttpJsonResponse(std::move(response));
                if (id == "cancel-chat")
                {
                    model.started_hold = true;
                    drogon::app().getLoop()->runAfter(std::chrono::milliseconds(1200),
                                                      [send = std::move(send), output]
                                                      {
                                                          send(output);
                                                      });
                }
                else
                {
                    send(output);
                }
            },
            {drogon::Post});
        app.registerHandler("/tts",
                            [&model](const drogon::HttpRequestPtr& request,
                                     std::function<void(const drogon::HttpResponsePtr&)>&& send)
                            {
#if defined(_WIN32)
                                if (request->body().contains(audition_text) &&
                                    !model.training_audition.wait())
                                {
                                    auto response = drogon::HttpResponse::newHttpResponse();
                                    response->setStatusCode(drogon::k504GatewayTimeout);
                                    send(response);
                                    return;
                                }
#else
                                (void)model;
                                (void)request;
#endif
                                constexpr unsigned frames = 800;
                                std::string bytes;
                                const auto u16 = [&bytes](unsigned value)
                                {
                                    bytes.push_back(static_cast<char>(value & 0xff));
                                    bytes.push_back(static_cast<char>(value >> 8));
                                };
                                const auto u32 = [&u16](unsigned value)
                                {
                                    u16(value & 0xffff);
                                    u16(value >> 16);
                                };
                                bytes += "RIFF";
                                u32(36 + frames * 2);
                                bytes += "WAVEfmt ";
                                u32(16);
                                u16(1);
                                u16(1);
                                u32(8000);
                                u32(16000);
                                u16(2);
                                u16(16);
                                bytes += "data";
                                u32(frames * 2);
                                for (unsigned index = 0; index < frames; ++index)
                                {
                                    u16(index % 40 < 20 ? 5000 : 60536);
                                }
                                auto output = drogon::HttpResponse::newHttpResponse();
                                output->setBody(std::move(bytes));
                                send(output);
                            },
                            {drogon::Post});
    }

    void check_training_control(unsigned short port, bool busy)
    {
        const auto training = body(tests::request(port, "/api/training"), 200);
        require(training["busy"] == busy, "Training status did not report the expected busy state");
        require(body(tests::request(port, "/api/training/cancel", "POST", {},
                                    R"({"id":"00000000-0000-0000-0000-000000000001"})"),
                     409)["code"] == "training_failed",
                "Training cancel must reach the training service while the backend waits");
    }

    void check_model_probe_isolation(unsigned short port, unsigned short fixture_port,
                                     FakeModel& model)
    {
        const auto input = std::format(
            R"({{"provider":"custom","api_format":"openai_chat","base_url":"http://127.0.0.1:{}/v1"}})",
            fixture_port);
        auto listing =
            std::async(std::launch::async,
                       [&]
                       {
                           return tests::request(port, "/api/llm/models", "POST", {}, input);
                       });
        model.model_listing.during(
            [&]
            {
                const auto rejected =
                    body(tests::request(port, "/api/llm/models", "POST", {}, input), 503);
                require(rejected["code"] == "llm_busy",
                        "The model probe queue did not enforce capacity");
                check_training_control(port, false);
                require(body(tests::request(port, "/api/live/settings", "POST", {}, "{}"),
                             400)["code"] == "invalid_live_settings",
                        "Live settings must remain available during a model listing");
                require(listing.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
                        "The model listing finished before the provider was released");
            });
        require(body(listing.get(), 200)["models"][0u]["id"] == "held-model",
                "The accepted model listing must complete after release");
    }

#if defined(_WIN32)
    void prepare_audition(const std::filesystem::path& directory)
    {
        constexpr std::string_view weight = "verified test weight";
        constexpr std::string_view digest =
            "aec8b9ed2b826b354e3b518219b791b5b041e4f3239f6d087a969cd8aa413cd4";
        const auto job = directory / "data" / "training" / "jobs" / audition_id;
        const auto artifacts = job / "artifacts";
        std::filesystem::create_directories(artifacts);
        tests::process::write_file(
            job / "status.json",
            std::format(
                R"({{"id":"{}","name":"Fixture","voice_id":"default","model_version":"v2","status":"completed","message":"","progress":100,"clip_count":1,"created_at_ms":1,"updated_at_ms":1}})",
                audition_id));
        tests::process::write_file(
            artifacts / "manifest.json",
            std::format(
                R"({{"job_id":"{}","model_version":"v2","sha256":{{"gpt":"{}","sovits":"{}"}}}})",
                audition_id, digest, digest));
        tests::process::write_file(artifacts / "gpt.ckpt", weight);
        tests::process::write_file(artifacts / "sovits.pth", weight);
        const auto default_gpt = directory / "default-gpt.ckpt";
        const auto default_sovits = directory / "default-sovits.pth";
        tests::process::write_file(default_gpt, weight);
        tests::process::write_file(default_sovits, weight);
        require(::_wputenv_s(L"MEOWLIVE_MANAGED_DEFAULT_GPT_WEIGHTS", default_gpt.c_str()) == 0 &&
                    ::_wputenv_s(L"MEOWLIVE_MANAGED_DEFAULT_SOVITS_WEIGHTS",
                                 default_sovits.c_str()) == 0,
                "Could not configure the controlled synthesis backend");
    }

    void check_audition_isolation(unsigned short port, FakeModel& model)
    {
        constexpr std::string_view path = "/api/training/audition";
        const auto input =
            std::format(R"({{"version_id":"{}","text":"{}"}})", audition_id, audition_text);
        auto audition = std::async(std::launch::async,
                                   [&]
                                   {
                                       return tests::request(port, path, "POST", {}, input);
                                   });
        model.training_audition.during(
            [&]
            {
                check_training_control(port, true);
                require(body(tests::request(port, path, "POST", {}, input), 503)["code"] ==
                            "training_busy",
                        "The audition queue did not enforce capacity");
                const auto deletion = std::format(R"({{"id":"{}"}})", audition_id);
                require(body(tests::request(port, "/api/training/delete", "POST", {}, deletion),
                             409)["code"] == "training_failed",
                        "An in-use audition version must stay available");
                require(audition.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
                        "Audition finished before synthesis was released");
            });
        const auto response = audition.get();
        require(response.status == 200 && response.body.starts_with("RIFF"),
                "The audition must return WAV after synthesis is released");
        check_training_control(port, false);
    }
#endif

    void run(const std::filesystem::path& executable)
    {
        const auto fixture_port = tests::process::available_loopback_port();
        FakeModel model;
        auto port = tests::process::available_loopback_port();
        while (port == fixture_port)
        {
            port = tests::process::available_loopback_port();
        }
        const tests::process::TestDirectory directory;
#if defined(_WIN32)
        prepare_audition(directory.path);
#endif
        std::ofstream(directory.path / "reference.wav") << "test reference path";
        std::ofstream(directory.path / "server.toml")
            << std::format(R"(
[server]
listen_address = "127.0.0.1:{}"
queue_capacity = 1
[agent]
cooldown_ms = 1000
[llm]
provider = "custom"
base_url = "http://127.0.0.1:{}/v1"
model = "fixed"
[speech]
base_url = "http://127.0.0.1:{}"
reference_audio = "reference.wav"
prompt_text = "test"
)",
                           port, fixture_port, fixture_port);
        tests::process::ChildProcess server;
        server.start(executable, directory.path, "server.toml", true);
        install_fixture(fixture_port, model, directory.path);
        const std::jthread fixture(
            []
            {
                drogon::app().run();
            });
        struct QuitFixture
        {
            ~QuitFixture()
            {
                drogon::app().quit();
            }
        } quit;
        try
        {
            wait(
                [&]() -> ::Json::Value
                {
                    server.assert_running();
                    try
                    {
                        const auto response = tests::request(port, "/api/health");
                        if (response.status == 200)
                        {
                            return body(response, 200);
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                    return {};
                },
                "Server did not start");
        }
        catch (...)
        {
            std::ifstream log(directory.path / "server.log");
            std::println(stderr, "Stage 4 service startup output: {}",
                         std::string(std::istreambuf_iterator<char>(log), {}));
            throw;
        }

        check_model_probe_isolation(port, fixture_port, model);
#if defined(_WIN32)
        check_audition_isolation(port, model);
#endif
        const auto query = [port]
        {
            return body(tests::request(port, "/api/agent"), 200);
        };
        const auto post = [port](std::string_view path, std::optional<std::string_view> json = {},
                                 unsigned status = 200)
        {
            return body(tests::request(port, path, "POST", {}, json), status);
        };
        require(query()["paused"].asBool(), "Agent must start paused");
        tests::WebSocket control(port, wire::control_path);
        control.send(1, wire::encode_client_message(wire::ClientHello{wire::version}));
        const auto hello = next_command<wire::ServerHello>(control);
        tests::WebSocket audio(port, std::format("/ws/audio?session_id={}&bridge_id={}",
                                                 hello.session_id, hello.bridge_id));
        require(audio.response().status == 101, "Audio channel did not pair");
        post("/api/agent/resume");
        const auto demo = event("demo-chat-1");
        require(post("/api/events", demo, 202)["accepted"] == 1, "Event not accepted");
        require(post("/api/events", demo, 202)["duplicates"] == 1, "Duplicate was accepted");
        send_obs_status(control);
        wait(
            [&]
            {
                const auto state = query();
                const auto row = find_event(state, "demo-chat-1");
                if (row["status"] == "failed")
                {
                    throw std::runtime_error("First decision failed: " + row["error"].asString());
                }
                return row["status"] == "ready" ? row : ::Json::Value{};
            },
            "First decision did not dispatch");
        require(model.saw_obs_status.load(), "Model continuation did not receive OBS status");
        const auto speak = next_command<wire::ServerSpeak>(control);
        require(!audio.wait_frame(2).empty(), "Speech PCM missing");
        send_receipt(control, speak, wire::ExecutionStatus::Started);
        wait(
            [&]
            {
                auto row = find_event(query(), "demo-chat-1");
                return row["status"] == "playing" ? row : ::Json::Value{};
            },
            "Agent did not observe playing");
        send_receipt(control, speak, wire::ExecutionStatus::Completed);
        send_receipt(control, speak, wire::ExecutionStatus::Completed);
        wait(
            [&]
            {
                auto state = query();
                return state["history"].size() == 1 ? state : ::Json::Value{};
            },
            "Completion did not update history exactly once");
        const auto usage = body(tests::request(port, "/api/llm/usage"), 200);
        require(usage["records"].size() == 2 && usage["total_tokens"] == 56, "Tool usage missing");
        const auto traces = body(tests::request(port, "/api/agent/traces"), 200);
        const auto trace_id = traces["traces"][0u]["id"].asString();
        require(body(tests::request(port, "/api/agent/traces/" + trace_id), 200)["state"] ==
                    "completed",
                "Trace detail is missing the completed playback");

        post("/api/events", event("next-chat"), 202);
        const auto following = next_command<wire::ServerSpeak>(control);
        static_cast<void>(audio.wait_frame(2));
        require(model.saw_history.load(), "Next model turn did not receive completed dialog");
        const auto stopped = post("/api/stop");
        const auto stop = next_command<wire::ServerStop>(control);
        require(stop.generation > following.generation &&
                    stopped["generation"].asUInt() == stop.generation,
                "Stop did not advance generation");
        send_receipt(control, following, wire::ExecutionStatus::Started);
        send_receipt(control, following, wire::ExecutionStatus::Completed);
        wait(
            [&]
            {
                auto state = query();
                return find_event(state, "next-chat")["status"] == "cancelled" &&
                               state["history"].size() == 1
                           ? state
                           : ::Json::Value{};
            },
            "Stop or late receipt changed committed dialog");

        post("/api/agent/resume");
        post("/api/events", event("bad-chat"), 202);
        wait(
            [&]
            {
                auto row = find_event(query(), "bad-chat");
                return row["status"] == "failed" ? row : ::Json::Value{};
            },
            "Invalid event ID did not fail the decision");
        post("/api/events", event("cancel-chat"), 202);
        wait(
            [&]() -> ::Json::Value
            {
                return model.started_hold.load() ? ::Json::Value(true) : ::Json::Value{};
            },
            "Model was not waiting");
        post("/api/agent/pause");
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
        const auto state = query();
        require(state["paused"].asBool() && state["history"].size() == 1 &&
                    find_event(state, "cancel-chat")["status"] == "pending",
                "Paused Agent applied a delayed model result");
        std::println("Agent event/tool/playback, deduplication, history, validation and "
                     "cancellation passed.");
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
        const auto self = std::filesystem::absolute(std::filesystem::path(argv[0]));
        run(argc == 2 ? std::filesystem::absolute(std::filesystem::path(argv[1])) :
#if defined(_WIN32)
                      self.parent_path() / "meowlive-server.exe"
#else
                      self.parent_path() / "meowlive-server"
#endif
        );
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Stage 4 Agent verification failed: {}", error.what());
        return 1;
    }
}
