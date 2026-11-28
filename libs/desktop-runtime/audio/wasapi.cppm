module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module meowlive.desktop_runtime.audio.wasapi;

import meowlive.platform.exception_diagnostics;
import meowlive.protocol.audio;
import meowlive.protocol.execution;

namespace
{
    class AudioEndpointError final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    void check(HRESULT result, std::string_view operation)
    {
        if (result == E_OUTOFMEMORY || result == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY))
        {
            throw std::bad_alloc{};
        }
        if (FAILED(result))
        {
            throw AudioEndpointError(std::format("{} failed (HRESULT 0x{:08x})", operation,
                                                 static_cast<std::uint32_t>(result)));
        }
    }

    class Apartment
    {
    public:
        Apartment()
        {
            check(::CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize audio COM");
        }

        ~Apartment()
        {
            ::CoUninitialize();
        }
    };

    class RenderStream
    {
    public:
        explicit RenderStream(meowlive::protocol::AudioFormat format) : format_(format)
        {
            ::Microsoft::WRL::ComPtr<::IMMDeviceEnumerator> enumerator;
            check(::CoCreateInstance(__uuidof(::MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(enumerator.GetAddressOf())),
                  "Create audio endpoint enumerator");
            ::Microsoft::WRL::ComPtr<::IMMDevice> device;
            check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()),
                  "Open default render endpoint");
            check(device->Activate(__uuidof(::IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(client_.GetAddressOf())),
                  "Activate WASAPI render stream");

            ::WAVEFORMATEX wave{
                .wFormatTag = WAVE_FORMAT_PCM,
                .nChannels = format.channels,
                .nSamplesPerSec = format.sample_rate,
                .nAvgBytesPerSec = format.sample_rate * format.channels * 2,
                .nBlockAlign = static_cast<WORD>(format.channels * 2),
                .wBitsPerSample = 16,
                .cbSize = 0,
            };
            // Shared-mode conversion maps the protocol rate and channels to the endpoint mix.
            check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      600'000, 0, &wave, nullptr),
                  "Initialize WASAPI render stream");
            check(client_->GetBufferSize(&capacity_), "Read WASAPI buffer capacity");
            check(client_->GetService(IID_PPV_ARGS(render_.GetAddressOf())),
                  "Open WASAPI render buffer");
            check(client_->GetService(IID_PPV_ARGS(clock_.GetAddressOf())),
                  "Open WASAPI playback clock");
            check(clock_->GetFrequency(&frequency_), "Read WASAPI clock frequency");
            if (capacity_ == 0 || frequency_ == 0)
            {
                throw AudioEndpointError("WASAPI returned an empty buffer or clock");
            }
        }

        ~RenderStream()
        {
            if (running_)
            {
                client_->Stop();
                client_->Reset();
            }
        }

        std::uint32_t padding()
        {
            UINT32 frames{};
            check(client_->GetCurrentPadding(&frames), "Read WASAPI render padding");
            return frames;
        }

        std::uint64_t position()
        {
            if (!running_)
            {
                return 0;
            }
            UINT64 ticks{};
            check(clock_->GetPosition(&ticks, nullptr), "Read WASAPI playback clock");
            return static_cast<std::uint64_t>(static_cast<long double>(ticks) *
                                              format_.sample_rate / frequency_);
        }

        void write(const std::vector<std::int16_t>& samples)
        {
            const auto frames = static_cast<UINT32>(samples.size() / format_.channels);
            BYTE* buffer{};
            check(render_->GetBuffer(frames, &buffer), "Acquire WASAPI render buffer");
            std::memcpy(buffer, samples.data(), samples.size() * sizeof(std::int16_t));
            check(render_->ReleaseBuffer(frames, 0), "Commit WASAPI render buffer");
            if (!running_)
            {
                check(client_->Start(), "Start WASAPI render stream");
                running_ = true;
            }
        }

        std::uint32_t capacity() const
        {
            return capacity_;
        }

    private:
        meowlive::protocol::AudioFormat format_;
        ::Microsoft::WRL::ComPtr<::IAudioClient> client_;
        ::Microsoft::WRL::ComPtr<::IAudioRenderClient> render_;
        ::Microsoft::WRL::ComPtr<::IAudioClock> clock_;
        UINT32 capacity_{};
        UINT64 frequency_{};
        bool running_{};
    };
    class RenderEnvelope
    {
    public:
        void submit(std::uint64_t start, const std::vector<std::int16_t>& samples,
                    meowlive::protocol::AudioFormat format)
        {
            const auto window_frames = std::max(1u, format.sample_rate / 100);
            const auto frames = samples.size() / format.channels;
            for (std::size_t offset = 0; offset < frames;)
            {
                const auto end = offset + std::min<std::size_t>(window_frames, frames - offset);
                double power{};
                for (auto index = offset * format.channels; index < end * format.channels; ++index)
                {
                    const auto sample = static_cast<double>(samples[index]) / 32768.0;
                    power += sample * sample;
                }
                windows_.push_back({start + offset, start + end,
                                    std::sqrt(power / ((end - offset) * format.channels))});
                offset = end;
            }
        }

        double at(std::uint64_t position)
        {
            while (!windows_.empty() && windows_.front().end <= position)
            {
                windows_.pop_front();
            }
            return !windows_.empty() && windows_.front().start <= position ? windows_.front().rms
                                                                           : 0.0;
        }

    private:
        struct Window
        {
            std::uint64_t start;
            std::uint64_t end;
            double rms;
        };
        std::deque<Window> windows_;
    };

}

export namespace meowlive::desktop_runtime::audio
{
    struct RenderEvent
    {
        meowlive::protocol::ExecutionStatus status;
        std::optional<std::string> error;
    };

    class WasapiOutput
    {
    public:
        WasapiOutput()
            : worker_(
                  [this](std::stop_token token)
                  {
                      this->run(token);
                  })
        {
        }

        ~WasapiOutput()
        {
            worker_.request_stop();
            changed_.notify_one();
        }

        void start(meowlive::protocol::AudioFormat format)
        {
            std::scoped_lock lock(mutex_);
            work_.emplace(format);
            events_.clear();
            level_.store(0.0, std::memory_order_relaxed);
            ++revision_;
            changed_.notify_one();
        }

        bool push(std::vector<std::int16_t> samples, bool end)
        {
            std::scoped_lock lock(mutex_);
            // Limit PCM awaiting the device. The worker releases each frame as it submits it.
            constexpr std::size_t capacity = (32 * 1024 * 1024) / sizeof(std::int16_t);
            if (!work_ || samples.size() > capacity - work_->sample_count)
            {
                return false;
            }
            if (!samples.empty())
            {
                work_->sample_count += samples.size();
                work_->chunks.push_back(std::move(samples));
            }
            work_->end = end;
            changed_.notify_one();
            return true;
        }

        void stop()
        {
            std::scoped_lock lock(mutex_);
            work_.reset();
            events_.clear();
            level_.store(0.0, std::memory_order_relaxed);
            ++revision_;
            changed_.notify_one();
        }

        double level() const noexcept
        {
            return level_.load(std::memory_order_relaxed);
        }

        std::vector<meowlive::desktop_runtime::audio::RenderEvent> poll()
        {
            std::scoped_lock lock(mutex_);
            return std::exchange(events_, {});
        }

    private:
        struct Work
        {
            explicit Work(meowlive::protocol::AudioFormat requested_format)
                : format(requested_format)
            {
            }

            meowlive::protocol::AudioFormat format;
            std::deque<std::vector<std::int16_t>> chunks;
            std::size_t sample_count{};
            std::size_t offset{};
            bool end{};
        };

        void run(std::stop_token token)
        {
            while (!token.stop_requested())
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, token,
                              [this]
                              {
                                  return work_ && !work_->chunks.empty();
                              });
                if (token.stop_requested())
                {
                    return;
                }
                const auto revision = revision_;
                const auto format = work_->format;
                lock.unlock();

                try
                {
                    const Apartment apartment;
                    this->render(token, revision, format);
                }
                catch (const AudioEndpointError& error)
                {
                    meowlive::platform::log_current_exception("WASAPI render endpoint");
                    fail_render(revision, error.what());
                }
                catch (...)
                {
                    const auto reason =
                        meowlive::platform::log_current_exception("WASAPI render worker");
                    using Kind = meowlive::platform::ExceptionKind;
                    const char* failure = "wasapi_internal_error";
                    if (reason == Kind::OutOfMemory)
                    {
                        failure = "wasapi_out_of_memory";
                    }
                    else if (reason == Kind::LogicError)
                    {
                        failure = "wasapi_logic_error";
                    }
                    fail_render(revision, failure);
                }
            }
        }

        void fail_render(std::uint64_t revision, const char* error)
        {
            std::scoped_lock lock(mutex_);
            if (revision == revision_)
            {
                work_.reset();
                level_.store(0.0, std::memory_order_relaxed);
                events_.push_back(
                    {meowlive::protocol::ExecutionStatus::Failed, std::string(error)});
            }
        }

        void render(std::stop_token token, std::uint64_t revision,
                    meowlive::protocol::AudioFormat format)
        {
            RenderStream stream(format);
            std::vector<std::int16_t> samples;
            samples.reserve(static_cast<std::size_t>(stream.capacity()) * format.channels);
            RenderEnvelope envelope;
            std::uint64_t submitted_end{};
            bool started{};
            while (!token.stop_requested())
            {
                const auto position = stream.position();
                const auto padding = stream.padding();
                if (padding > stream.capacity())
                {
                    throw AudioEndpointError("WASAPI padding exceeds buffer capacity");
                }
                std::unique_lock lock(mutex_);
                if (revision != revision_)
                {
                    return;
                }
                level_.store(position > 0 && padding > 0 ? envelope.at(position) : 0.0,
                             std::memory_order_relaxed);
                if (position > 0 && !started)
                {
                    events_.push_back({meowlive::protocol::ExecutionStatus::Started, std::nullopt});
                    started = true;
                }

                samples.clear();
                const auto space =
                    static_cast<std::size_t>(stream.capacity() - padding) * format.channels;
                while (!work_->chunks.empty() && samples.size() < space)
                {
                    auto& chunk = work_->chunks.front();
                    const auto count =
                        std::min(chunk.size() - work_->offset, space - samples.size());
                    const auto first = chunk.begin() + static_cast<std::ptrdiff_t>(work_->offset);
                    samples.insert(samples.end(), first,
                                   first + static_cast<std::ptrdiff_t>(count));
                    work_->offset += count;
                    work_->sample_count -= count;
                    if (work_->offset == chunk.size())
                    {
                        work_->chunks.pop_front();
                        work_->offset = 0;
                    }
                }

                if (samples.empty() && work_->end && padding == 0 && started &&
                    position >= submitted_end)
                {
                    events_.push_back(
                        {meowlive::protocol::ExecutionStatus::Completed, std::nullopt});
                    work_.reset();
                    return;
                }
                if (!samples.empty() && !token.stop_requested())
                {
                    // If network input pauses, the clock can advance past the last submitted PCM.
                    const auto first_frame = std::max(submitted_end, position + padding);
                    submitted_end = first_frame + samples.size() / format.channels;
                    envelope.submit(first_frame, samples, format);
                    // Keep the final buffer submission ordered with stop and revision changes.
                    stream.write(samples);
                }
                changed_.wait_for(lock, token, std::chrono::milliseconds(5),
                                  [this, revision]
                                  {
                                      return revision != revision_;
                                  });
            }
        }

        std::mutex mutex_;
        std::condition_variable_any changed_;
        std::optional<Work> work_;
        std::vector<meowlive::desktop_runtime::audio::RenderEvent> events_;
        std::uint64_t revision_{};
        std::atomic<double> level_{};
        std::jthread worker_;
    };
}
