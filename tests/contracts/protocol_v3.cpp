#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.protocol.control;

namespace
{
    void require(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    void check_control()
    {
        constexpr std::string_view client_hello = R"({"protocol_version":3,"type":"hello"})";
        constexpr std::string_view server_hello =
            R"({"bridge_id":"bridge-1","generation":1,"protocol_version":3,"session_id":"session-1","type":"hello"})";
        constexpr std::string_view server_speak =
            R"({"format":{"channels":1,"sample_rate":32000},"generation":1,"type":"speak","utterance_id":"speech-1"})";
        constexpr std::string_view server_stop = R"({"generation":2,"type":"stop"})";
        constexpr std::string_view client_completed =
            R"({"receipt":{"error":null,"generation":1,"status":"completed","utterance_id":"speech-1"},"type":"receipt"})";

        const auto client = meowlive::protocol::decode_client_message(client_hello);
        require(client &&
                    std::get<meowlive::protocol::ClientHello>(*client).protocol_version == 3 &&
                    meowlive::protocol::encode_client_message(*client) == client_hello,
                "client hello fixture");
        const auto server = meowlive::protocol::decode_server_command(server_hello);
        require(server &&
                    std::get<meowlive::protocol::ServerHello>(*server).session_id == "session-1" &&
                    meowlive::protocol::encode_server_command(*server) == server_hello,
                "server hello fixture");
        const auto speak = meowlive::protocol::decode_server_command(server_speak);
        require(speak &&
                    std::get<meowlive::protocol::ServerSpeak>(*speak).format ==
                        meowlive::protocol::AudioFormat{32'000, 1} &&
                    meowlive::protocol::encode_server_command(*speak) == server_speak,
                "server speak fixture");
        const auto stop = meowlive::protocol::decode_server_command(server_stop);
        require(stop && std::get<meowlive::protocol::ServerStop>(*stop).generation == 2 &&
                    meowlive::protocol::encode_server_command(*stop) == server_stop,
                "server stop fixture");
        const auto receipt = meowlive::protocol::decode_client_message(client_completed);
        require(receipt &&
                    std::get<meowlive::protocol::ExecutionReceipt>(*receipt).status ==
                        meowlive::protocol::ExecutionStatus::Completed &&
                    meowlive::protocol::encode_client_message(*receipt) == client_completed,
                "execution receipt fixture");
        for (const auto status : {meowlive::protocol::ExecutionStatus::Started,
                                  meowlive::protocol::ExecutionStatus::Cancelled,
                                  meowlive::protocol::ExecutionStatus::Failed})
        {
            const meowlive::protocol::ExecutionReceipt sent{"speech-\xe4\xb8\xad", 1, status,
                                                            "message \"quoted\""};
            const auto received = meowlive::protocol::decode_client_message(
                meowlive::protocol::encode_client_message(sent));
            require(received && std::get<meowlive::protocol::ExecutionReceipt>(*received) == sent,
                    "receipt round trip");
        }
        require(
            meowlive::protocol::decode_client_message(R"({"type":"hello","protocol_version":2})")
                .has_value(),
            "transport owns hello protocol negotiation");
        constexpr std::array<std::string_view, 11> invalid_clients{
            R"({"type":"hello","protocol_version":65536})",
            R"({"type":"hello","protocol_version":-1})",
            R"({"type":"hello","protocol_version":3.0})",
            R"({"type":"hello","protocol_version":03})",
            R"({"type":"hello","protocol_version":"3"})",
            R"({"type":"hello","protocol_version":3,"protocol_version":4})",
            R"({"type":"hello","protocol_version":3}{})",
            R"({"type":"receipt","receipt":{"error":0,"status":"started","utterance_id":"x","generation":0}})",
            R"({"type":"receipt","receipt":{"status":"unknown","utterance_id":"x","generation":0}})",
            R"({"type":"receipt","receipt":{"status":"started","utterance_id":"\udc00","generation":0}})",
            R"({"type":"receipt","receipt":{"status":"started","utterance_id":"\ud800\u0020","generation":0}})",
        };
        for (const auto input : invalid_clients)
        {
            require(!meowlive::protocol::decode_client_message(input),
                    "invalid client message accepted");
        }
        const auto unicode_receipt = meowlive::protocol::decode_client_message(
            R"({"type":"receipt","receipt":{"status":"started","utterance_id":"\ud83d\udc08","generation":0}})");
        require(unicode_receipt &&
                    std::get<meowlive::protocol::ExecutionReceipt>(*unicode_receipt).utterance_id ==
                        "\xf0\x9f\x90\x88",
                "Valid JSON surrogate pair did not decode to UTF-8");
        require(!meowlive::protocol::decode_server_command(
                    R"({"type":"stop","generation":4294967296})"),
                "server generation overflow accepted");
        require(!meowlive::protocol::decode_server_command(
                    R"({"type":"speak","generation":0,"format":[]})"),
                "invalid server audio format accepted");
    }

    void check_json_depth()
    {
        const auto opening = R"({"type":"stop","generation":1,"extra":)" + std::string(127, '[');
        const auto closing = std::string(127, ']') + '}';
        require(meowlive::protocol::decode_server_command(opening + closing).has_value(),
                "empty JSON container at depth limit");
        require(!meowlive::protocol::decode_server_command(opening + '0' + closing),
                "JSON value beyond depth limit accepted");
        require(!meowlive::protocol::decode_server_command(opening + '}' + closing.substr(1)),
                "mismatched JSON delimiter at depth limit accepted");
    }

    void check_resources()
    {
        constexpr std::array<std::string_view, 10> operations{
            R"({"type":"obs_settings"})",
            R"({"settings":{"clear_password":false,"enabled":true,"password":null,"websocket_url":"ws://127.0.0.1:4455"},"type":"save_obs_settings"})",
            R"({"operation":{"scene_name":"studio","type":"set_scene"},"type":"obs"})",
            R"({"type":"list_models"})",
            R"({"model_id":"avatar","mouth_parameter":"open","type":"load_model"})",
            R"({"model_id":"avatar","type":"list_hotkeys"})",
            R"({"fallback_hotkey_id":null,"hotkey_id":"wave","model_id":"avatar","type":"trigger_hotkey"})",
            R"({"type":"import_model"})",
            R"({"type":"list_imported_models"})",
            R"({"id":"avatar","type":"delete_imported_model"})",
        };
        for (const auto operation : operations)
        {
            const auto wire = "{\"operation\":" + std::string(operation) +
                              R"(,"request_id":"r","type":"resource"})";
            const auto command = meowlive::protocol::decode_server_command(wire);
            require(command &&
                        std::get<meowlive::protocol::ResourceCommand>(*command).request_id == "r" &&
                        meowlive::protocol::encode_server_command(*command) == wire,
                    "desktop resource operation round trip");
        }

        constexpr std::array<std::string_view, 10> results{
            R"({"settings":{"enabled":true,"password_configured":true,"storage_available":true,"websocket_url":"ws://127.0.0.1:4455"},"type":"obs_settings"})",
            R"({"snapshot":{"connected":true,"current_scene":"studio","recording":false,"scenes":["studio"]},"type":"obs"})",
            R"({"models":[{"id":"avatar","name":"Avatar"}],"type":"models"})",
            R"({"model_id":"avatar","type":"model_loaded"})",
            R"({"hotkeys":[{"id":"wave","name":"Wave"}],"model_id":"avatar","type":"hotkeys"})",
            R"({"hotkey_id":"wave","type":"hotkey_triggered"})",
            R"({"bytes":100,"files":2,"model_file":"avatar.model3.json","model_name":"Avatar","restart_required":true,"type":"model_imported"})",
            R"({"models":[{"id":"avatar","model_id":null,"name":"Avatar"}],"type":"imported_models"})",
            R"({"id":"avatar","restart_required":false,"type":"model_deleted"})",
            R"({"code":"unavailable","message":"offline","type":"error"})",
        };
        for (const auto result : results)
        {
            const auto wire = R"({"request_id":"r","result":)" + std::string(result) +
                              R"(,"type":"resource_result"})";
            const auto message = meowlive::protocol::decode_client_message(wire);
            require(message &&
                        std::get<meowlive::protocol::ResourceResultMessage>(*message).request_id ==
                            "r" &&
                        meowlive::protocol::encode_client_message(*message) == wire,
                    "desktop resource result round trip");
        }
        require(
            !meowlive::protocol::decode_server_command(
                R"({"type":"resource","request_id":"r","operation":{"type":"obs","operation":{"type":"status","extra":true}}})"),
            "unexpected OBS operation field accepted");
        require(
            !meowlive::protocol::decode_client_message(
                R"({"type":"resource_result","request_id":"r","result":{"type":"models","models":[1]}})"),
            "invalid models array accepted");
        const auto local_import = meowlive::protocol::decode_server_command(
            R"({"type":"resource","request_id":"r","operation":{"type":"import_model","path":"external"}})");
        require(
            local_import &&
                meowlive::protocol::encode_server_command(*local_import) ==
                    R"({"operation":{"type":"import_model"},"request_id":"r","type":"resource"})",
            "local import unit variant ignores incoming fields");
    }

    void check_audio()
    {
        constexpr std::array<std::uint8_t, 31> frame{
            0x4d, 0x4c, 0x50, 0x43, 0x03, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00,
            0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
            0x61, 0x00, 0x80, 0xff, 0xff, 0x00, 0x00, 0xff, 0x7f,
        };
        const meowlive::protocol::AudioChunk expected{"a", 7, 2, {-32768, -1, 0, 32767}, true};
        const auto decoded = meowlive::protocol::decode_audio_chunk(frame);
        const auto encoded = meowlive::protocol::encode_audio_chunk(expected);
        require(decoded && *decoded == expected && encoded && std::ranges::equal(*encoded, frame),
                "Rust MLPC fixture");

        for (const auto offset : {std::size_t{0}, std::size_t{4}, std::size_t{6}, std::size_t{16},
                                  std::size_t{18}, std::size_t{22}})
        {
            auto broken = frame;
            broken[offset] = 0xff;
            require(!meowlive::protocol::decode_audio_chunk(broken), "invalid MLPC field accepted");
        }
        auto wrong_size = std::vector(frame.begin(), frame.end());
        wrong_size.push_back(0);
        require(!meowlive::protocol::decode_audio_chunk(wrong_size),
                "audio trailing data accepted");
        wrong_size.resize(21);
        require(!meowlive::protocol::decode_audio_chunk(wrong_size),
                "truncated audio header accepted");

        meowlive::protocol::AudioChunk maximum{
            std::string(meowlive::protocol::max_utterance_id_bytes, 'a'),
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            std::vector<std::int16_t>(meowlive::protocol::max_audio_samples, 32767),
            true,
        };
        const auto maximum_frame = meowlive::protocol::encode_audio_chunk(maximum);
        require(maximum_frame &&
                    maximum_frame->size() == meowlive::protocol::max_audio_frame_bytes &&
                    meowlive::protocol::decode_audio_chunk(*maximum_frame) == maximum,
                "maximum-size audio frame");
        maximum.samples.push_back(1);
        require(!meowlive::protocol::encode_audio_chunk(maximum), "oversized PCM accepted");
        maximum.samples.clear();
        maximum.utterance_id.push_back('x');
        require(!meowlive::protocol::encode_audio_chunk(maximum),
                "oversized audio identity accepted");
        maximum.utterance_id.clear();
        require(!meowlive::protocol::encode_audio_chunk(maximum), "empty audio identity accepted");
        maximum.utterance_id = "\xed\xa0\x80";
        require(!meowlive::protocol::encode_audio_chunk(maximum), "UTF-8 surrogate accepted");
        maximum.utterance_id = "\xf0\x9f\x98\x80";
        const auto empty_end = meowlive::protocol::encode_audio_chunk(maximum);
        require(empty_end && meowlive::protocol::decode_audio_chunk(*empty_end) == maximum,
                "empty PCM end frame");
        require(meowlive::protocol::valid_audio_format({8'000, 1}) &&
                    meowlive::protocol::valid_audio_format({96'000, 2}) &&
                    !meowlive::protocol::valid_audio_format({7'999, 1}) &&
                    !meowlive::protocol::valid_audio_format({96'001, 2}) &&
                    !meowlive::protocol::valid_audio_format({32'000, 3}),
                "PCM sample rate or channel bounds");
    }
}

int main()
{
    try
    {
        check_control();
        check_json_depth();
        check_resources();
        check_audio();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println("protocol v3 contract failed: {}", error.what());
        return 1;
    }
}
