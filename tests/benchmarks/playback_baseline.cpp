#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

import meowlive.protocol.json;
import meowlive.tests.audio_loopback;
import meowlive.tests.controlled_tts;
import meowlive.tests.server_process;
import meowlive.tests.transport_client;

namespace
{
    using Clock = std::chrono::steady_clock;
    using Process = meowlive::tests::process::ChildProcess;

    constexpr std::array latency_fields{"start_to_http_ms", "client_start_to_paired_ms",
                                        "enqueue_to_tone_ms", "stop_to_silence_ms"};
    constexpr std::string_view fixture_sha256{
        "209489fb2e950c9da1e1c7790865091f446166797b8c5475a60afd1fe46f5c1f"};

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    double rounded(double value, double scale)
    {
        return std::round(value * scale) / scale;
    }

    double milliseconds(Clock::time_point start, Clock::time_point end)
    {
        return rounded(std::chrono::duration<double, std::milli>(end - start).count(), 10);
    }

    void wait_health(std::uint16_t port, const Process& server, const Process* client)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        while (Clock::now() < deadline)
        {
            server.assert_running();
            if (client)
            {
                client->assert_running();
            }
            try
            {
                const auto response = meowlive::tests::request(port, "/api/health");
                if (response.status == 200)
                {
                    const auto body = meowlive::protocol::json::parse_object(response.body);
                    if (body && (*body)["protocol_version"].asInt() == 3 &&
                        (*body)["bridge_connected"].isBool() &&
                        (*body)["bridge_connected"].asBool() == (client != nullptr))
                    {
                        return;
                    }
                }
            }
            catch (const std::system_error&)
            {
                // The server may still be opening its listener.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error(client ? "Executor did not pair"
                                        : "Server did not become healthy");
    }

    ::Json::Value post(std::uint16_t port, std::string_view path, const ::Json::Value& body,
                       int expected_status)
    {
        const auto payload = meowlive::protocol::json::serialize(body);
        const auto response = meowlive::tests::request(port, path, "POST", {}, payload);
        require(response.status == expected_status,
                std::format("{} returned HTTP {}, expected {}", path, response.status,
                            expected_status));
        auto result = meowlive::protocol::json::parse_object(response.body);
        if (path != "/api/events")
        {
            require(result.has_value(), std::format("{} did not return a JSON object", path));
        }
        return result.value_or(::Json::Value{});
    }

    void write_trial_files(const std::filesystem::path& path, std::uint16_t port,
                           const meowlive::tests::ControlledTts& tts)
    {
        const auto config = std::format(
            "[server]\nlisten_address = \"127.0.0.1:{}\"\n"
            "[speech]\nbase_url = \"http://127.0.0.1:{}\"\nreference_audio = \"reference.wav\"\n"
            "prompt_text = \"{}\"\n[resources]\ndirectory = \"resources\"\n"
            "[auth]\nenabled = false\n[live]\nenabled = false\n"
            "[viewers]\nenabled = false\n[training]\nenabled = false\n",
            port, tts.port(), meowlive::tests::playback_text);
        meowlive::tests::process::write_file(path / "reference.wav", tts.wav());
        meowlive::tests::process::write_file(path / "server.toml", config);
        meowlive::tests::process::write_file(
            path / "desktop.toml", std::format("server_url = \"http://127.0.0.1:{}\"\n", port));
    }

    template <typename Probe> Clock::time_point wait_audio(Probe probe, std::string_view failure)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        while (Clock::now() < deadline)
        {
            if (auto observed = probe())
            {
                return *observed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error(std::string(failure));
    }

    void sample_memory(const std::array<const Process*, 2>& processes,
                       std::array<std::uint64_t, 2>& peaks, std::chrono::milliseconds duration)
    {
        const auto deadline = Clock::now() + duration;
        do
        {
            for (std::size_t index = 0; index < processes.size(); ++index)
            {
                peaks[index] =
                    std::max(peaks[index], processes[index]->metrics().working_set_bytes);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (Clock::now() < deadline);
    }

    ::Json::Value measure_trial(const std::filesystem::path& server_exe,
                                const std::filesystem::path& client_exe,
                                const meowlive::tests::ControlledTts& tts,
                                meowlive::tests::AudioLoopback& audio)
    {
        const meowlive::tests::process::TestDirectory directory;
        const auto port = meowlive::tests::process::available_loopback_port();
        write_trial_files(directory.path, port, tts);
        Process server;
        Process client;
        const std::array<const Process*, 2> processes{&server, &client};
        std::array<std::uint64_t, 2> peaks{};

        const auto started = Clock::now();
        server.start(server_exe, directory.path, "server.toml");
        wait_health(port, server, nullptr);
        const auto healthy = Clock::now();
        const auto url = std::format(L"http://127.0.0.1:{}", port);
        require(::SetEnvironmentVariableW(L"MEOWLIVE_SERVER_URL", url.c_str()) &&
                    ::SetEnvironmentVariableW(L"MEOWLIVE_DATA_DIR", directory.path.c_str()),
                "Cannot set the isolated execution endpoint");
        client.start(client_exe, directory.path, "desktop.toml", false, "client.log");
        wait_health(port, server, &client);
        const auto paired = Clock::now();

        const std::string text(meowlive::tests::playback_text);
        ::Json::Value event(::Json::objectValue);
        event["id"] = "stage1-chat";
        event["source"] = "simulator";
        event["viewer"] = "viewer";
        event["kind"]["type"] = "chat";
        event["kind"]["text"] = text;
        ::Json::Value events(::Json::objectValue);
        events["events"].append(event);
        post(port, "/api/events", events, 202);
        sample_memory(processes, peaks, std::chrono::milliseconds(300));
        require(!audio.audible_after(paired), "Test tone was present before enqueue");

        const std::array before{server.metrics(), client.metrics()};
        ::Json::Value request(::Json::objectValue);
        request["text"] = text;
        request["voice_id"] = "default";
        const auto enqueued = Clock::now();
        const auto speech = post(port, "/api/speech", request, 202);
        require(speech["status"].asString() == "queued", "Speech was not queued");
        const auto audible = wait_audio(
            [&]
            {
                return audio.audible_after(enqueued);
            },
            "Render endpoint did not play the test tone");
        sample_memory(processes, peaks, std::chrono::milliseconds(500));

        const auto stop = Clock::now();
        const auto stopped = post(port, "/api/stop", ::Json::Value(::Json::objectValue), 200);
        require(stopped["speeches"].isArray() && !stopped["speeches"].empty() &&
                    stopped["speeches"][0]["status"].asString() == "cancelled",
                "Stopped speech did not reach cancelled status");
        const auto silent = wait_audio(
            [&]
            {
                return audio.silent_after(stop);
            },
            "Render endpoint did not become silent");
        sample_memory(processes, peaks, std::chrono::milliseconds(200));
        require(!audio.audible_after(silent), "Test tone returned after stop");

        ::Json::Value result(::Json::objectValue);
        result[latency_fields[0]] = milliseconds(started, healthy);
        result[latency_fields[1]] = milliseconds(healthy, paired);
        result[latency_fields[2]] = milliseconds(enqueued, audible);
        result[latency_fields[3]] = milliseconds(stop, silent);
        for (std::size_t index = 0; index < processes.size(); ++index)
        {
            const auto after = processes[index]->metrics();
            result["working_set_peak_mib"].append(rounded(peaks[index] / (1024.0 * 1024.0), 100));
            result["cpu_time_ms"].append(
                rounded((after.cpu_seconds - before[index].cpu_seconds) * 1000, 10));
        }
        return result;
    }

    unsigned parse_trials(std::wstring_view value)
    {
        unsigned trials{};
        for (const auto digit : value)
        {
            require(digit >= L'0' && digit <= L'9' &&
                        trials <= (1'000u - static_cast<unsigned>(digit - L'0')) / 10,
                    "--trials must be an integer between 1 and 1000");
            trials = trials * 10 + static_cast<unsigned>(digit - L'0');
        }
        require(trials > 0, "--trials must be positive");
        return trials;
    }

    void report(const std::filesystem::path& server, const std::filesystem::path& client,
                unsigned trials)
    {
        const meowlive::tests::process::TestDirectory tts_directory;
        const meowlive::tests::ControlledTts tts(tts_directory.path);
        meowlive::tests::AudioLoopback audio;
        const auto wav = tts.wav();
        ::Json::Value result(::Json::objectValue);
        result["render_endpoint"] = audio.endpoint();
        auto digest = drogon::utils::getSha256(wav.data(), wav.size());
        for (auto& digit : digest)
        {
            if (digit >= 'A' && digit <= 'F')
            {
                digit = static_cast<char>(digit - 'A' + 'a');
            }
        }
        require(digest == fixture_sha256, "Generated WAV does not match the Stage 1 fixture");
        result["wav_sha256"] = digest;
        for (unsigned trial = 0; trial < trials; ++trial)
        {
            try
            {
                result["runs"].append(measure_trial(server, client, tts, audio));
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error(std::format("Trial {}: {}", trial + 1, error.what()));
            }
        }
        for (const auto* field : latency_fields)
        {
            std::vector<double> observed;
            for (const auto& run : result["runs"])
            {
                observed.push_back(run[field].asDouble());
            }
            std::ranges::sort(observed);
            result["latency_median_ms"][field] =
                (observed[(trials - 1) / 2] + observed[trials / 2]) / 2;
        }
        ::Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::println("{}", ::Json::writeString(writer, result));
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        constexpr std::string_view usage{
            "meowlive-playback-baseline SERVER.exe CLIENT.exe [--trials NUMBER]"};
        if (argc == 2 && std::wstring_view(argv[1]) == L"--help")
        {
            std::println("{}", usage);
            return 0;
        }
        require(argc == 3 || (argc == 5 && std::wstring_view(argv[3]) == L"--trials"), usage);
        report(std::filesystem::canonical(argv[1]), std::filesystem::canonical(argv[2]),
               argc == 5 ? parse_trials(argv[4]) : 5);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Playback baseline failed: {}", error.what());
        return 1;
    }
}
