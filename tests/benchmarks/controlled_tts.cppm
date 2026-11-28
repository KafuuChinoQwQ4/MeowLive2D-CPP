module;

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.tests.controlled_tts;

import meowlive.standard;
import meowlive.protocol.json;

export namespace meowlive::tests
{
    inline constexpr std::string_view playback_text = "跨端闭环";
    inline constexpr std::string_view delayed_playback_text = "等待合成";
    inline constexpr std::string_view invalid_wave_text = "损坏音频";
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

    struct SpeechFixture
    {
        explicit SpeechFixture(const std::filesystem::path& file) : reference(file)
        {
        }

        const std::filesystem::path reference;
        std::function<void()> release;
        std::atomic<bool> delayed{};

        bool valid_reference(const ::Json::Value& value) const
        {
            if (!value.isString())
            {
                return false;
            }
            const auto path = value.asString();
            return std::filesystem::path(std::u8string(path.begin(), path.end())) == reference;
        }

        void respond(const drogon::HttpRequestPtr& request, const std::string& wav,
                     std::function<void(const drogon::HttpResponsePtr&)> callback)
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            const auto payload = meowlive::protocol::json::parse_object(request->body());
            if (request->path() != "/tts" || !payload || !(*payload)["text"].isString() ||
                !valid_reference((*payload)["ref_audio_path"]) ||
                (*payload)["prompt_text"] != meowlive::tests::playback_text.data() ||
                (*payload)["prompt_lang"] != "zh" || (*payload)["text_lang"] != "zh" ||
                (*payload)["media_type"] != "wav")
            {
                response->setStatusCode(drogon::k400BadRequest);
                callback(response);
                return;
            }

            const auto& text = (*payload)["text"];
            response->setStatusCode(drogon::k200OK);
            response->setContentTypeString("audio/wav");
            if (text == meowlive::tests::invalid_wave_text.data())
            {
                response->setBody("invalid wave");
            }
            else
            {
                response->setBody(wav);
            }
            if (text == meowlive::tests::delayed_playback_text.data() && !release)
            {
                release = [response, callback = std::move(callback)]
                {
                    callback(response);
                };
                delayed.store(true);
                return;
            }
            callback(response);
        }
    };
}

export namespace meowlive::tests
{
    class ControlledTts
    {
    public:
        explicit ControlledTts(const std::filesystem::path& scratch_directory,
                               const std::filesystem::path& reference_file = {})
            : wav_(std::make_shared<const std::string>(meowlive::tests::fixture_wav())),
              speech_(reference_file.empty()
                          ? nullptr
                          : std::make_shared<meowlive::tests::SpeechFixture>(reference_file))
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
                [wav = wav_,
                 speech = speech_](const drogon::HttpRequestPtr& request,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback)
                {
                    if (speech)
                    {
                        speech->respond(request, *wav, std::move(callback));
                    }
                    else
                    {
                        callback(meowlive::tests::fixture_response(request, *wav));
                    }
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

        bool has_delayed_response() const
        {
            return speech_ && speech_->delayed.load();
        }

        void release_delayed_response() const
        {
            drogon::app().getIOLoop(0)->queueInLoop(
                [speech = speech_]
                {
                    if (auto release = std::exchange(speech->release, {}))
                    {
                        speech->delayed.store(false);
                        release();
                    }
                });
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
        std::shared_ptr<meowlive::tests::SpeechFixture> speech_;
        std::thread worker_;
        std::uint16_t port_{};
    };
}
