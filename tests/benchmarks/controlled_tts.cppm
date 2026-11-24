module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>

#if defined(_WIN32)
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#endif

export module meowlive.tests.controlled_tts;

#if !defined(_WIN32)
import std;
#endif
import meowlive.protocol.json;

export namespace meowlive::tests
{
    inline constexpr std::string_view playback_text = "跨端闭环";
}

namespace meowlive::tests
{
    std::string fixture_wav()
    {
        constexpr std::uint32_t rate = 24000;
        constexpr std::uint32_t frames = rate * 4;
        constexpr std::uint32_t bytes = frames * 2;
        constexpr std::array frequencies{900, 1700, 2500, 3900};

        std::string wav{"RIFF"};
        wav.reserve(44 + bytes);
        const auto append_le = [&wav](std::uint32_t value, unsigned width)
        {
            for (unsigned byte = 0; byte < width; ++byte)
            {
                wav.push_back(static_cast<char>(value & 0xff));
                value >>= 8;
            }
        };
        append_le(36 + bytes, 4);
        wav += "WAVEfmt ";
        append_le(16, 4);
        append_le(1, 2); // PCM
        append_le(1, 2); // Mono
        append_le(rate, 4);
        append_le(rate * 2, 4);
        append_le(2, 2);
        append_le(16, 2);
        wav += "data";
        append_le(bytes, 4);

        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            double composite = 0.0;
            for (const auto frequency : frequencies)
            {
                composite += std::sin(2.0 * std::numbers::pi * frequency * frame / rate);
            }
            append_le(static_cast<std::uint16_t>(std::lround(1800.0 * composite)), 2);
        }
        return wav;
    }

    drogon::HttpResponsePtr fixture_response(const drogon::HttpRequestPtr& request,
                                             const std::string& wav)
    {
        auto response = drogon::HttpResponse::newHttpResponse();
        const auto payload = meowlive::protocol::json::parse_object(request->body());
        if (request->path() == "/tts" && payload && (*payload)["text"].isString() &&
            (*payload)["text"].asString() == meowlive::tests::playback_text)
        {
            response->setStatusCode(drogon::k200OK);
            response->setContentTypeString("audio/wav");
            response->setBody(wav);
        }
        else
        {
            response->setStatusCode(drogon::k400BadRequest);
        }
        return response;
    }
}

export namespace meowlive::tests
{
    class ControlledTts
    {
    public:
        explicit ControlledTts(const std::filesystem::path& scratch_directory)
            : wav_(std::make_shared<const std::string>(meowlive::tests::fixture_wav()))
        {
            auto ready = std::make_shared<std::promise<std::uint16_t>>();
            auto started = ready->get_future();
            auto& app = drogon::app();
            app.disableSigtermHandling()
                .setThreadNum(1)
                .setLogLevel(trantor::Logger::kWarn)
                .setFileTypes({})
                .setImplicitPageEnable(false)
                .enableGzip(false)
                .setClientMaxBodySize(64 * 1024)
                .setIdleConnectionTimeout(3)
                .setUploadPath(drogon::utils::fromNativePath(scratch_directory.native()))
                .addListener("127.0.0.1", 0);
            app.registerHandlerViaRegex(
                "/.*",
                [wav = wav_](const drogon::HttpRequestPtr& request,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback)
                {
                    callback(meowlive::tests::fixture_response(request, *wav));
                },
                {drogon::Post});
            app.registerBeginningAdvice(
                [ready]
                {
                    const auto addresses = drogon::app().getListeners();
                    ready->set_value(addresses.empty() ? 0 : addresses.front().toPort());
                });
            worker_ = std::thread(
                [ready]
                {
                    try
                    {
                        drogon::app().run();
                    }
                    catch (...)
                    {
                        try
                        {
                            ready->set_exception(std::current_exception());
                        }
                        catch (const std::future_error&)
                        {
                            // The application already reported its startup result.
                        }
                    }
                });
            try
            {
                if (started.wait_for(std::chrono::seconds(5)) != std::future_status::ready ||
                    (port_ = started.get()) == 0)
                {
                    throw std::runtime_error("Controlled TTS did not start on loopback");
                }
            }
            catch (...)
            {
                this->stop();
                throw;
            }
        }

        ~ControlledTts()
        {
            this->stop();
        }

        std::uint16_t port() const
        {
            return port_;
        }

        std::string_view wav() const
        {
            return *wav_;
        }

    private:
        void stop()
        {
            drogon::app().getLoop()->queueInLoop(
                []
                {
                    drogon::app().quit();
                });
            worker_.join();
        }

        std::shared_ptr<const std::string> wav_;
        std::thread worker_;
        std::uint16_t port_{};
    };
}
