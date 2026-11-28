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

import meowlive.standard;

import meowlive.protocol.control;
import meowlive.protocol.json;
import meowlive.tests.controlled_tts;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

#if defined(_WIN32)
import meowlive.tests.audio_loopback;
#endif

import meowlive.tests.playback_fixture;

namespace meowlive::tests::playback
{
    void check_http(const PlaybackServer& server)
    {
        expect_error(meowlive::tests::request(server.port, speech_path, "POST", {}, speech_body()),
                     401, "authentication_required");
        expect_error(meowlive::tests::request(server.port, stop_path, "POST",
                                              meowlive::tests::device_credential),
                     401, "authentication_required");
        expect_error(server.post(speech_path), 400, "invalid_request");
        expect_error(server.post(speech_path, speech_body(), "text/plain"), 400, "invalid_request");
        expect_error(server.post(speech_path, R"({"text":1,"voice_id":"default"})"), 400,
                     "invalid_request");
        auto oversized = speech_body();
        oversized.resize(16 * 1024 + 1, ' ');
        expect_error(server.post(speech_path, oversized), 400, "invalid_request");
        expect_error(server.post(speech_path, speech_body(" ")), 400, "invalid_text");
        expect_error(server.post(speech_path, speech_body("有效文本", "")), 400, "invalid_voice");
        expect_error(server.post(speech_path, speech_body("有效文本", "unused")), 400,
                     "unknown_voice");
        expect_error(server.post(speech_path, speech_body()), 409, "bridge_disconnected");
        expect_error(server.post(preview_path), 409, "bridge_disconnected");
    }

    std::vector<::Json::Value> read_journal(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        std::vector<::Json::Value> entries;
        for (std::string line; std::getline(stream, line) && !stream.eof();)
        {
            auto entry = meowlive::protocol::json::parse_object(line);
            require(entry.has_value(), "Terminal journal contained invalid JSON");
            entries.push_back(std::move(*entry));
        }
        return entries;
    }

    void await_journal(const PlaybackServer& server, std::string_view id)
    {
        await(
            [&]
            {
                const auto entries = read_journal(server.journal_file());
                return std::ranges::any_of(entries,
                                           [&](const auto& entry)
                                           {
                                               return entry["utterance_id"].asString() == id;
                                           });
            },
            "Expected completion absent from terminal journal");
    }

    void check_playback(const PlaybackServer& server)
    {
        using meowlive::protocol::ExecutionStatus;
        std::string first_id;
        std::string second_id;
        std::string dispatched;
        std::string pending;
        std::uint32_t initial_generation{};
        {
            Executor client(server);
            initial_generation = client.hello.generation;
            auto generation = initial_generation;

            const auto pcm = server.wav().substr(44);
            first_id = server.enqueue();
            const auto first = client.receive_pcm(first_id, generation, pcm);
            server.wait_status(first_id, "ready");
            client.receipt(first, ExecutionStatus::Started);
            server.wait_status(first_id, "playing");
            client.receipt(first, ExecutionStatus::Completed);
            server.wait_status(first_id, "completed");
            client.receipt(first, ExecutionStatus::Completed);
            const auto completed_first = response_body(server.post(stop_path), 200);
            require(find_speech(completed_first, first_id)["status"] == "completed",
                    "Stop replaced an accepted completion");
            require(completed_first["generation"].asUInt() == ++generation,
                    "Stop did not advance generation");
            client.expect_stop(generation);

            second_id = server.enqueue();
            const auto second = client.receive_pcm(second_id, generation, pcm);
            client.receipt(second, ExecutionStatus::Started);
            server.wait_status(second_id, "playing");
            const auto stopped = response_body(server.post(stop_path), 200);
            require(find_speech(stopped, second_id)["status"] == "cancelled" &&
                        stopped["generation"].asUInt() == ++generation,
                    "Stopping a playing speech did not cancel it and advance generation");
            client.expect_stop(generation);
            client.receipt(second, ExecutionStatus::Completed);

            const auto delayed = server.enqueue(meowlive::tests::delayed_playback_text);
            server.wait_delayed();
            server.wait_status(delayed, "synthesizing");
            const auto stopped_during_synthesis = response_body(server.post(stop_path), 200);
            require(find_speech(stopped_during_synthesis, delayed)["status"] == "cancelled" &&
                        stopped_during_synthesis["generation"].asUInt() == ++generation,
                    "Stopping while synthesis was held did not cancel it");
            client.expect_stop(generation);
            server.release_delayed();

            const auto failed = server.enqueue(meowlive::tests::invalid_wave_text);
            server.wait_status(failed, "failed");
            require(find_speech(server.status(), second_id)["status"] == "cancelled",
                    "A stale completed receipt replaced cancellation");
            const auto preview = response_body(server.post(preview_path), 202);
            const auto preview_id = preview["id"].asString();
            require(preview["voice_id"] == "preview" && preview["status"] == "queued",
                    "Preview did not use the packaged trial");
            const auto trial = client.receive_pcm(preview_id, generation, pcm);
            client.receipt(trial, ExecutionStatus::Started);
            client.receipt(trial, ExecutionStatus::Completed);
            server.wait_status(preview_id, "completed");

            dispatched = server.enqueue();
            client.receive_pcm(dispatched, generation, pcm);
            pending = server.enqueue();
            server.wait_status(pending, "queued");
            expect_error(server.post(speech_path, speech_body()), 429, "queue_full");
        }
        server.wait_connected(false);
        server.wait_status(dispatched, "unknown");
        server.wait_status(pending, "cancelled");

        Executor reconnected(server);
        require(reconnected.hello.generation > initial_generation,
                "Reconnect accepted an old playback generation");
        const auto resumed_id = server.enqueue();
        const auto resumed = reconnected.receive_pcm(resumed_id, reconnected.hello.generation,
                                                     server.wav().substr(44));
        reconnected.receipt(resumed, ExecutionStatus::Started);
        reconnected.receipt(resumed, ExecutionStatus::Completed);
        server.wait_status(resumed_id, "completed");
        await_journal(server, resumed_id);
        unsigned completions{};
        for (const auto& entry : read_journal(server.journal_file()))
        {
            const auto id = entry["utterance_id"].asString();
            require(id != second_id, "Journal accepted stale completion after stop");
            if (id == first_id)
            {
                require(entry["status"] == "completed" &&
                            entry["generation"].asUInt() == initial_generation,
                        "Journal recorded incorrect first terminal result");
                ++completions;
            }
        }
        require(completions == 1, "Duplicate completion was accepted or completion was lost");
    }

    std::string complete_speech(const PlaybackServer& server, Executor& client,
                                std::uint32_t generation)
    {
        const auto id = server.enqueue();
        const auto speech = client.receive_pcm(id, generation, server.wav().substr(44));
        client.receipt(speech, meowlive::protocol::ExecutionStatus::Started);
        client.receipt(speech, meowlive::protocol::ExecutionStatus::Completed);
        server.wait_status(id, "completed");
        return id;
    }

    void check_receipt_storage(const PlaybackServer& server)
    {
        Executor client(server);
        auto generation = client.hello.generation;
        const auto file = server.journal_file();
        const auto backup = file.parent_path() / "accepted.jsonl";
        std::filesystem::rename(file, backup);
#if !defined(_WIN32)
        require(::mkfifo(file.c_str(), 0600) == 0, "Cannot delay receipt journal with a FIFO");
        const auto delayed = complete_speech(server, client, generation);
        const auto stopped = response_body(server.post(stop_path), 200);
        require(find_speech(stopped, delayed)["status"] == "completed",
                "Pending journal write blocked stop or replaced accepted completion");
        client.expect_stop(++generation);
        {
            struct FifoReader
            {
                const int descriptor;
                ~FifoReader()
                {
                    if (descriptor >= 0)
                    {
                        ::close(descriptor);
                    }
                }
            };
            const FifoReader reader{::open(file.c_str(), O_RDONLY | O_NONBLOCK)};
            require(reader.descriptor >= 0, "Cannot release delayed journal writer");
            std::string line;
            await(
                [&]
                {
                    std::array<char, 256> bytes;
                    const auto count = ::read(reader.descriptor, bytes.data(), bytes.size());
                    if (count > 0)
                    {
                        line.append(bytes.data(), static_cast<std::size_t>(count));
                    }
                    return line.ends_with('\n');
                },
                "Accepted terminal result was not written after releasing delayed storage");
            const auto entry = meowlive::protocol::json::parse_object(line);
            require(entry && (*entry)["utterance_id"].asString() == delayed &&
                        (*entry)["status"] == "completed",
                    "Delayed journal output disagreed with the accepted terminal result");
        }
        std::filesystem::remove(file);
#endif
        std::filesystem::create_directory(file);
        const auto failed = complete_speech(server, client, generation);
        await(
            [&]
            {
                return server.logged_write_failure(failed);
            },
            "Terminal journal write failure was not logged");
        const auto stopped_after_failure = response_body(server.post(stop_path), 200);
        require(find_speech(stopped_after_failure, failed)["status"] == "completed",
                "Journal write failure changed the accepted terminal result");
        client.expect_stop(++generation);
        std::filesystem::remove(file);
        std::filesystem::rename(backup, file);
        await_journal(server, complete_speech(server, client, generation));
    }

#if defined(_WIN32)
    void check_native(const PlaybackServer& server, const std::filesystem::path& desktop_exe)
    {
        using Clock = std::chrono::steady_clock;
        const auto url = std::format(L"http://127.0.0.1:{}", server.port);
        const std::wstring device(meowlive::tests::device_credential.begin(),
                                  meowlive::tests::device_credential.end());
        require(::SetEnvironmentVariableW(L"MEOWLIVE_SERVER_URL", url.c_str()) &&
                    ::SetEnvironmentVariableW(L"MEOWLIVE_DEVICE_TOKEN", device.c_str()),
                "Cannot configure native Qt desktop connection");
        const meowlive::tests::AudioLoopback audio;
        const meowlive::tests::process::TestDirectory directory;
        meowlive::tests::process::ChildProcess desktop;
        desktop.start(desktop_exe, directory.path, "desktop.toml", false, "desktop.log");
        server.wait_connected(true);
        const auto preview_started_at = Clock::now();
        const auto preview = response_body(server.post(preview_path), 202);
        const auto preview_id = preview["id"].asString();
        await(
            [&]
            {
                return audio.audible_after(preview_started_at).has_value();
            },
            "Windows render endpoint did not output the packaged preview");
        server.wait_status(preview_id, "playing");
        server.wait_status(preview_id, "completed", std::chrono::seconds(6));
        require(Clock::now() - preview_started_at >= std::chrono::milliseconds(3500),
                "Preview receipt arrived before its four-second PCM could finish");
        const auto preview_completed = Clock::now();
        await(
            [&]
            {
                return audio.silent_after(preview_completed).has_value();
            },
            "Windows loopback did not settle after preview completed");

        const auto enqueued = Clock::now();
        const auto stopped_id = server.enqueue();
        std::optional<Clock::time_point> audible;
        await(
            [&]
            {
                desktop.assert_running();
                audible = audio.audible_after(enqueued);
                return audible.has_value();
            },
            "Windows render endpoint did not output the fixed PCM");
        server.wait_status(stopped_id, "playing");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto stop = Clock::now();
        const auto snapshot = response_body(server.post(stop_path), 200);
        require(find_speech(snapshot, stopped_id)["status"] == "cancelled",
                "Windows playback did not cancel at stop");
        std::optional<Clock::time_point> silent;
        await(
            [&]
            {
                silent = audio.silent_after(stop);
                return silent.has_value();
            },
            "Windows render endpoint did not become silent after stop");
        constexpr auto silence_observation = std::chrono::milliseconds(250);
        std::this_thread::sleep_for(silence_observation);
        require(!audio.audible_after(*silent), "Stopped PCM became audible again");

        const auto resumed_at = Clock::now();
        const auto resumed_id = server.enqueue();
        await(
            [&]
            {
                return audio.audible_after(resumed_at).has_value();
            },
            "Windows render endpoint did not output PCM after stop");
        server.wait_status(resumed_id, "playing");
        server.wait_status(resumed_id, "completed", std::chrono::seconds(6));
        const auto stop_ms = std::chrono::duration<double, std::milli>(*silent - stop).count();
        std::println("Native Qt preview and PCM verified on {}. Stop to silence {:.1f} ms.",
                     audio.endpoint(), stop_ms);
    }
#endif

    template <typename Char> void run(int argc, Char** argv)
    {
#if defined(_WIN32)
        require(argc <= 3, "meowlive-playback-integration [SERVER [DESKTOP]]");
#else
        require(argc <= 2, "meowlive-playback-integration [SERVER]");
#endif
        const auto executable =
            argc >= 2 ? std::filesystem::path(argv[1])
                      : std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path() /
#if defined(_WIN32)
                            "meowlive-server.exe";
#else
                            "meowlive-server";
#endif
        const PlaybackServer server(executable);
        check_http(server);
        check_playback(server);
        server.wait_connected(false);
        check_receipt_storage(server);
        std::println(
            "Speech HTTP, fixed PCM, receipts, cancellation, reconnection and journal passed.");
#if defined(_WIN32)
        if (argc == 3)
        {
            server.wait_connected(false);
            check_native(server, std::filesystem::path(argv[2]));
        }
#endif
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
        meowlive::tests::playback::run(argc, argv);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Playback integration failed: {}", error.what());
        return 1;
    }
}
