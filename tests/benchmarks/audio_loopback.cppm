module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module meowlive.tests.audio_loopback;

namespace meowlive::tests::loopback_detail
{
    using Clock = std::chrono::steady_clock;
    constexpr std::chrono::milliseconds block_time{10};
    constexpr std::array frequencies{900, 1700, 2500, 3900};
    constexpr double threshold = 0.018;

    void check(HRESULT result, std::string_view operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(std::format("{} failed (HRESULT 0x{:08x})", operation,
                                                 static_cast<std::uint32_t>(result)));
        }
    }

    class Apartment
    {
    public:
        Apartment()
        {
            check(::CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
        }

        ~Apartment()
        {
            ::CoUninitialize();
        }
    };

    struct PropertyValue
    {
        PROPVARIANT value{};

        ~PropertyValue()
        {
            ::PropVariantClear(&value);
        }
    };

    std::string device_name(::IMMDevice& device)
    {
        ::Microsoft::WRL::ComPtr<::IPropertyStore> properties;
        check(device.OpenPropertyStore(STGM_READ, properties.GetAddressOf()),
              "Open endpoint properties");
        PropertyValue property;
        check(properties->GetValue(::PKEY_Device_FriendlyName, &property.value),
              "Read endpoint name");
        if (property.value.vt != VT_LPWSTR || !property.value.pwszVal)
        {
            throw std::runtime_error("Default render endpoint has no friendly name");
        }

        const std::wstring_view wide(property.value.pwszVal);
        const auto length = static_cast<int>(wide.size());
        const auto bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                                                 nullptr, 0, nullptr, nullptr);
        if (bytes == 0)
        {
            throw std::runtime_error("Cannot encode render endpoint name as UTF-8");
        }
        std::string name(static_cast<std::size_t>(bytes), '\0');
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, name.data(),
                                  bytes, nullptr, nullptr) != bytes)
        {
            throw std::runtime_error("Cannot encode render endpoint name as UTF-8");
        }
        return name;
    }

    template <typename Sample> Sample read_sample(const BYTE* data)
    {
        Sample value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    template <typename Sample, unsigned Bits> double signed_pcm(const BYTE* data)
    {
        return static_cast<double>(read_sample<Sample>(data)) / (std::uint64_t{1} << (Bits - 1));
    }

    double pcm24(const BYTE* data)
    {
        const int raw =
            data[0] | (static_cast<int>(data[1]) << 8) | (static_cast<int>(data[2]) << 16);
        return static_cast<double>((raw ^ 0x800000) - 0x800000) / 8'388'608.0;
    }

    using DecodeSample = double (*)(const BYTE*);

    DecodeSample sample_decoder(const WAVEFORMATEX& format)
    {
        auto tag = format.wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22)
        {
            const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
            if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            {
                tag = WAVE_FORMAT_PCM;
            }
            else if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            {
                tag = WAVE_FORMAT_IEEE_FLOAT;
            }
        }

        if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32)
        {
            return [](const BYTE* data)
            {
                return static_cast<double>(read_sample<float>(data));
            };
        }
        if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 64)
        {
            return read_sample<double>;
        }
        if (tag == WAVE_FORMAT_PCM)
        {
            switch (format.wBitsPerSample)
            {
            case 8:
                return [](const BYTE* data)
                {
                    return (static_cast<double>(*data) - 128.0) / 128.0;
                };
            case 16:
                return signed_pcm<std::int16_t, 16>;
            case 24:
                return pcm24;
            case 32:
                return signed_pcm<std::int32_t, 32>;
            }
        }
        throw std::runtime_error("Unsupported WASAPI render sample format");
    }

    struct Quadrature
    {
        double real{};
        double imaginary{};
    };

    class ToneDetector
    {
    public:
        explicit ToneDetector(const WAVEFORMATEX& format)
            : frame_bytes_(format.nBlockAlign), sample_bytes_(format.wBitsPerSample / 8),
              decode_(sample_decoder(format)), basis_(format.nSamplesPerSec / 100),
              channels_(format.nChannels)
        {
            if (channels_.empty() || format.nSamplesPerSec <= 2 * frequencies.back() ||
                frame_bytes_ != channels_.size() * sample_bytes_)
            {
                throw std::runtime_error("WASAPI format cannot resolve the test tones");
            }
            for (std::size_t frame = 0; frame < basis_.size(); ++frame)
            {
                for (std::size_t tone = 0; tone < frequencies.size(); ++tone)
                {
                    const auto angle = 2.0 * std::numbers::pi * frequencies[tone] *
                                       static_cast<double>(frame) / format.nSamplesPerSec;
                    basis_[frame][tone] = {std::cos(angle), std::sin(angle)};
                }
            }
        }

        template <typename Consumer>
        void append(const BYTE* data, UINT32 frames, Consumer&& observe)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                if (data)
                {
                    const auto* sample = data + static_cast<std::size_t>(frame) * frame_bytes_;
                    for (auto& channel : channels_)
                    {
                        const auto value = decode_(sample);
                        sample += sample_bytes_;
                        for (std::size_t tone = 0; tone < frequencies.size(); ++tone)
                        {
                            channel[tone].real += value * basis_[position_][tone].real;
                            channel[tone].imaginary += value * basis_[position_][tone].imaginary;
                        }
                    }
                }
                if (++position_ == basis_.size())
                {
                    observe(amplitude());
                    reset();
                }
            }
        }

        void reset()
        {
            position_ = 0;
            std::ranges::fill(channels_, std::array<Quadrature, frequencies.size()>{});
        }

    private:
        double amplitude() const
        {
            std::array<double, frequencies.size()> maxima{};
            const auto scale = 2.0 / static_cast<double>(basis_.size());
            for (const auto& channel : channels_)
            {
                for (std::size_t tone = 0; tone < frequencies.size(); ++tone)
                {
                    const auto& value = channel[tone];
                    maxima[tone] =
                        std::max(maxima[tone], std::hypot(value.real, value.imaginary) * scale);
                }
            }
            return std::ranges::min(maxima);
        }

        std::size_t frame_bytes_;
        std::size_t sample_bytes_;
        DecodeSample decode_;
        std::vector<std::array<Quadrature, frequencies.size()>> basis_;
        std::vector<std::array<Quadrature, frequencies.size()>> channels_;
        std::size_t position_{};
    };

    struct Observation
    {
        Clock::time_point time;
        std::optional<double> amplitude;
    };
}

export namespace meowlive::tests
{
    class AudioLoopback
    {
    public:
        AudioLoopback()
            : worker_(
                  [this](std::stop_token stop)
                  {
                      capture(stop);
                  })
        {
            std::unique_lock lock(mutex_);
            if (!ready_signal_.wait_for(lock, std::chrono::seconds(5),
                                        [this]
                                        {
                                            return ready_ || failure_;
                                        }))
            {
                throw std::runtime_error("Timed out starting WASAPI loopback");
            }
            check_capture();
        }

        std::string endpoint() const
        {
            std::scoped_lock lock(mutex_);
            check_capture();
            return endpoint_;
        }

        std::optional<std::chrono::steady_clock::time_point>
        audible_after(std::chrono::steady_clock::time_point time) const
        {
            return find_streak(time, true, 2);
        }

        std::optional<std::chrono::steady_clock::time_point>
        silent_after(std::chrono::steady_clock::time_point time) const
        {
            return find_streak(time, false, 3);
        }

    private:
        void check_capture() const
        {
            if (failure_)
            {
                std::rethrow_exception(failure_);
            }
        }

        std::optional<std::chrono::steady_clock::time_point>
        find_streak(std::chrono::steady_clock::time_point time, bool audible,
                    unsigned required) const
        {
            std::scoped_lock lock(mutex_);
            check_capture();
            unsigned count{};
            std::chrono::steady_clock::time_point first;
            for (const auto& observation : observations_)
            {
                if (observation.time < time)
                {
                    continue;
                }
                if (!observation.amplitude ||
                    (*observation.amplitude >= meowlive::tests::loopback_detail::threshold) !=
                        audible)
                {
                    count = 0;
                    continue;
                }
                if (count++ == 0)
                {
                    first = observation.time;
                }
                if (count == required)
                {
                    return audible ? first : observation.time;
                }
            }
            return std::nullopt;
        }

        void observe(std::optional<double> amplitude)
        {
            std::scoped_lock lock(mutex_);
            observations_.push_back({std::chrono::steady_clock::now(), amplitude});
            if (observations_.size() > 2'000)
            {
                observations_.pop_front();
            }
        }

        void capture(std::stop_token stop)
        {
            try
            {
                capture_endpoint(stop);
            }
            catch (...)
            {
                {
                    std::scoped_lock lock(mutex_);
                    failure_ = std::current_exception();
                }
                ready_signal_.notify_one();
            }
        }

        void capture_endpoint(std::stop_token stop)
        {
            using meowlive::tests::loopback_detail::check;
            const meowlive::tests::loopback_detail::Apartment apartment;
            ::Microsoft::WRL::ComPtr<::IMMDeviceEnumerator> enumerator;
            check(::CoCreateInstance(__uuidof(::MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(enumerator.GetAddressOf())),
                  "Create audio endpoint enumerator");
            ::Microsoft::WRL::ComPtr<::IMMDevice> device;
            check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf()),
                  "Open default render endpoint");
            auto name = meowlive::tests::loopback_detail::device_name(*device.Get());

            ::Microsoft::WRL::ComPtr<::IAudioClient> client;
            check(device->Activate(__uuidof(::IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(client.GetAddressOf())),
                  "Activate WASAPI loopback");
            WAVEFORMATEX* format{};
            check(client->GetMixFormat(&format), "Read WASAPI mix format");
            const std::unique_ptr<WAVEFORMATEX, decltype(&::CoTaskMemFree)> format_owner(
                format, ::CoTaskMemFree);
            meowlive::tests::loopback_detail::ToneDetector detector(*format);
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     1'000'000, 0, format, nullptr),
                  "Initialize WASAPI loopback");
            REFERENCE_TIME period{};
            check(client->GetDevicePeriod(&period, nullptr), "Read render endpoint period");
            using HundredNanoseconds =
                std::chrono::duration<REFERENCE_TIME, std::ratio<1, 10'000'000>>;
            const auto idle_grace = std::max(HundredNanoseconds(std::chrono::milliseconds(20)),
                                             HundredNanoseconds(period) * 2);
            ::Microsoft::WRL::ComPtr<::IAudioCaptureClient> packets;
            check(client->GetService(IID_PPV_ARGS(packets.GetAddressOf())),
                  "Open WASAPI capture stream");
            check(client->Start(), "Start WASAPI capture stream");
            {
                std::scoped_lock lock(mutex_);
                endpoint_ = std::move(name);
                ready_ = true;
            }
            ready_signal_.notify_one();

            auto next_idle = std::chrono::steady_clock::now() + idle_grace;
            bool idle{};
            while (!stop.stop_requested())
            {
                UINT32 pending{};
                check(packets->GetNextPacketSize(&pending), "Query WASAPI capture packets");
                if (pending)
                {
                    BYTE* data{};
                    UINT32 frames{};
                    DWORD flags{};
                    check(packets->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                          "Read WASAPI capture packet");
                    if (frames)
                    {
                        if (idle || (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY))
                        {
                            detector.reset();
                            observe(std::nullopt);
                        }
                        idle = false;
                        detector.append((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data,
                                        frames,
                                        [this](double amplitude)
                                        {
                                            observe(amplitude);
                                        });
                        check(packets->ReleaseBuffer(frames), "Release WASAPI capture packet");
                        next_idle = std::chrono::steady_clock::now() + idle_grace;
                        continue;
                    }
                }

                // An idle render endpoint can stop delivering packets. Wait for two periods
                // before treating packet starvation as silence, then sample it every 10 ms.
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_idle)
                {
                    if (!idle)
                    {
                        detector.reset();
                        observe(std::nullopt);
                        idle = true;
                    }
                    observe(0.0);
                    next_idle = now + meowlive::tests::loopback_detail::block_time;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(client->Stop(), "Stop WASAPI loopback");
        }

        mutable std::mutex mutex_;
        std::condition_variable ready_signal_;
        std::string endpoint_;
        std::exception_ptr failure_;
        std::deque<meowlive::tests::loopback_detail::Observation> observations_;
        bool ready_{};
        std::jthread worker_;
    };
}
