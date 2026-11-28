module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <cstdio>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#endif
#include <meowlive/msvc_stl.hpp>

export module meowlive.tests.playback_fixture;

import meowlive.standard;

import meowlive.protocol.control;
import meowlive.protocol.json;
import meowlive.tests.controlled_tts;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

export namespace meowlive::tests::playback
{
    inline constexpr std::string_view speech_path = "/api/speech";
    inline constexpr std::string_view preview_path = "/api/speech/preview";
    inline constexpr std::string_view status_path = "/api/status";
    inline constexpr std::string_view stop_path = "/api/stop";
    inline constexpr std::string_view voice_id = "default";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    template <typename Condition>
    void await(Condition condition, std::string_view message,
               std::chrono::seconds limit = std::chrono::seconds(4))
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        do
        {
            if (condition())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error(std::string(message));
    }

    ::Json::Value response_body(const meowlive::tests::HttpResponse& response, int expected)
    {
        require(response.status == expected, std::format("HTTP {} expected but received {}: {}",
                                                         expected, response.status, response.body));
        const auto json = meowlive::protocol::json::parse_object(response.body);
        require(json.has_value(), "Expected a JSON response");
        return *json;
    }

    void expect_error(const meowlive::tests::HttpResponse& response, int code,
                      std::string_view error)
    {
        require(response_body(response, code)["code"].asString() == error, error);
    }

    std::string speech_body(std::string_view text = meowlive::tests::playback_text,
                            std::string_view voice = voice_id)
    {
        ::Json::Value request(::Json::objectValue);
        request["text"] = std::string(text);
        request["voice_id"] = std::string(voice);
        return meowlive::protocol::json::serialize(request);
    }

    const ::Json::Value& find_speech(const ::Json::Value& snapshot, std::string_view id)
    {
        for (const auto& speech : snapshot["speeches"])
        {
            if (speech["id"].asString() == id)
            {
                return speech;
            }
        }
        throw std::runtime_error(std::format("Speech {} absent from status", id));
    }

    class PlaybackServer
    {
    public:
        explicit PlaybackServer(const std::filesystem::path& executable)
            : tts_(directory_.path / "tts", directory_.path / "reference sample.wav")
        {
            meowlive::tests::process::write_file(directory_.path / "reference sample.wav",
                                                 tts_.wav());
            const auto config = std::format(
                "[server]\nlisten_address = \"127.0.0.1:{}\"\nqueue_capacity = 2\n"
                "[speech]\nbase_url = \"http://127.0.0.1:{}\"\n"
                "reference_audio = \"reference sample.wav\"\nprompt_text = \"{}\"\n"
                "timeout_seconds = 15\n[auth]\nenabled = true\n"
                "admin_token_env = \"\"\ndevice_token_env = \"\"\n"
                "admin_token_file = \"admin.token\"\ndevice_token_file = \"device.token\"\n",
                port, tts_.port(), meowlive::tests::playback_text);
            meowlive::tests::process::write_file(directory_.path / "server.toml", config);
            meowlive::tests::process::write_file(directory_.path / "admin.token",
                                                 meowlive::tests::admin_credential);
            meowlive::tests::process::write_file(directory_.path / "device.token",
                                                 meowlive::tests::device_credential);
            child_.start(executable, directory_.path, "server.toml", true);
            wait_connected(false);
            ::Json::Value request(::Json::objectValue);
            request["token"] = std::string(meowlive::tests::admin_credential);
            token_ = response_body(
                         meowlive::tests::request(port, "/api/admin/session", "POST", {},
                                                  meowlive::protocol::json::serialize(request)),
                         200)["token"]
                         .asString();
            require(!token_.empty(), "Administrator login did not return a session");
        }

        void wait_connected(bool connected) const
        {
            await(
                [&]
                {
                    child_.assert_running();
                    try
                    {
                        const auto response =
                            meowlive::tests::request(port, meowlive::protocol::health_path);
                        return response_body(response, 200)["bridge_connected"].asBool() ==
                               connected;
                    }
                    catch (const std::system_error&)
                    {
                        return false;
                    }
                },
                std::format("Bridge did not reach bridge_connected={}", connected),
                std::chrono::seconds(8));
        }

        meowlive::tests::HttpResponse post(std::string_view path,
                                           std::optional<std::string_view> body = {},
                                           std::string_view media_type = "application/json") const
        {
            return meowlive::tests::request(port, path, "POST", token_, body, {}, media_type);
        }

        ::Json::Value status() const
        {
            return response_body(meowlive::tests::request(port, status_path, "GET", token_), 200);
        }

        std::string enqueue(std::string_view text = meowlive::tests::playback_text) const
        {
            const auto task = response_body(post(speech_path, speech_body(text)), 202);
            require(task["status"] == "queued" && task["text"].asString() == text &&
                        task["voice_id"].asString() == voice_id && task["error"].isNull(),
                    "Unexpected queued speech snapshot");
            const auto id = task["id"].asString();
            require(!id.empty(), "Queued speech is missing ID");
            return id;
        }

        void wait_status(std::string_view id, std::string_view expected,
                         std::chrono::seconds limit = std::chrono::seconds(4)) const
        {
            std::string last_status;
            await(
                [&]
                {
                    const auto snapshot = status();
                    last_status = find_speech(snapshot, id)["status"].asString();
                    return last_status == expected;
                },
                std::format("Speech {} did not reach {}", id, expected), limit);
        }

        void wait_delayed() const
        {
            await(
                [&]
                {
                    return tts_.has_delayed_response();
                },
                "TTS did not receive delayed speech");
        }

        void release_delayed() const
        {
            tts_.release_delayed_response();
        }

        std::string_view wav() const
        {
            return tts_.wav();
        }

        std::filesystem::path journal_file() const
        {
            return directory_.path / "data" / "receipts" / "playback.jsonl";
        }

        bool logged_write_failure(std::string_view id) const
        {
            const auto logs = directory_.path / "data" / "logs";
            for (const auto& entry : std::filesystem::directory_iterator(logs))
            {
                std::ifstream stream(entry.path(), std::ios::binary);
                const std::string contents{std::istreambuf_iterator<char>(stream), {}};
                if (contents.contains(std::format("playback_receipt_write_failed {}", id)))
                {
                    return true;
                }
            }
            return false;
        }

    private:
        const meowlive::tests::process::TestDirectory directory_;
        const meowlive::tests::ControlledTts tts_;

    public:
        const std::uint16_t port = meowlive::tests::process::available_loopback_port();

    private:
        meowlive::tests::process::ChildProcess child_;
        std::string token_;
    };

    template <typename Command> Command read_command(meowlive::tests::WebSocket& control)
    {
        auto message = meowlive::protocol::decode_server_command(control.wait_frame(1));
        require(message && std::holds_alternative<Command>(*message),
                "Unexpected WebSocket control message");
        return std::get<Command>(std::move(*message));
    }

    class Executor
    {
    public:
        explicit Executor(const PlaybackServer& server)
            : control_(server.port, meowlive::protocol::control_path,
                       meowlive::tests::device_credential),
              hello(handshake()),
              audio_(server.port, pair_path(hello), meowlive::tests::device_credential)
        {
            require(audio_.response().status == 101, "Audio WebSocket upgrade failed");
            server.wait_connected(true);
        }

        meowlive::protocol::ServerSpeak receive_pcm(const std::string& id, std::uint32_t generation,
                                                    std::string_view expected_pcm = {})
        {
            auto speak = read_command<meowlive::protocol::ServerSpeak>(control_);
            require(speak.utterance_id == id && speak.generation == generation &&
                        meowlive::protocol::valid_audio_format(speak.format),
                    "Speak command is missing the expected ID, generation or format");
            std::string received;
            unsigned sequence{};
            for (;;)
            {
                const auto data = audio_.wait_frame(2);
                const auto frame = meowlive::protocol::decode_audio_chunk(
                    std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
                require(frame && frame->utterance_id == id && frame->generation == generation &&
                            frame->sequence == sequence++,
                        "MLPC frame was missing, out of order or assigned to the wrong speech");
                const auto payload_size = frame->samples.size() * 2;
                received.append(data.substr(data.size() - payload_size));
                require(received.size() <= 1024 * 1024, "Unexpectedly large controlled PCM stream");
                if (frame->end)
                {
                    break;
                }
            }
            require(!received.empty() && received.size() % (speak.format.channels * 2) == 0,
                    "Speech contained no usable PCM");
            if (!expected_pcm.empty())
            {
                require(speak.format == meowlive::protocol::AudioFormat{24000, 1} &&
                            received == expected_pcm && sequence > 1,
                        "Speech did not preserve the fixed PCM or split it into multiple frames");
            }
            return speak;
        }

        void receipt(const meowlive::protocol::ServerSpeak& speech,
                     meowlive::protocol::ExecutionStatus status)
        {
            control_.send(
                1, meowlive::protocol::encode_client_message(meowlive::protocol::ExecutionReceipt{
                       speech.utterance_id, speech.generation, status, {}}));
        }

        void expect_stop(std::uint32_t generation)
        {
            require(read_command<meowlive::protocol::ServerStop>(control_).generation == generation,
                    "Stop arrived with unexpected generation");
        }

    private:
        static std::string pair_path(const meowlive::protocol::ServerHello& hello)
        {
            return "/ws/audio?session_id=" + drogon::utils::urlEncodeComponent(hello.session_id) +
                   "&bridge_id=" + drogon::utils::urlEncodeComponent(hello.bridge_id);
        }

        meowlive::protocol::ServerHello handshake()
        {
            require(control_.response().status == 101, "Control WebSocket upgrade failed");
            control_.send(1, meowlive::protocol::encode_client_message(
                                 meowlive::protocol::ClientHello{meowlive::protocol::version}));
            return read_command<meowlive::protocol::ServerHello>(control_);
        }

        meowlive::tests::WebSocket control_;

    public:
        const meowlive::protocol::ServerHello hello;

    private:
        meowlive::tests::WebSocket audio_;
    };
}
