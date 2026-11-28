module;

#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.model_weights;

import meowlive.standard;

export namespace meowlive::adapters
{
    struct ModelWeights
    {
        std::filesystem::path gpt;
        std::filesystem::path sovits;
        bool operator==(const meowlive::adapters::ModelWeights&) const = default;
    };
}

namespace
{
    namespace adapters = meowlive::adapters;

    bool valid_file(const std::filesystem::path& file)
    {
        std::error_code error;
        return file.is_absolute() && std::filesystem::is_regular_file(file, error);
    }

    bool loopback_service(const std::string& address)
    {
        if (!address.starts_with("http://127.0.0.1:") && !address.starts_with("http://[::1]:"))
        {
            return false;
        }
        const auto endpoint = std::string_view(address).substr(address.rfind(':') + 1);
        return !endpoint.empty() &&
               endpoint.find_first_not_of("0123456789") == std::string_view::npos;
    }

    std::size_t discard_response(char*, std::size_t size, std::size_t count, void*) noexcept
    {
        return size * count;
    }

    int cancel_weight_load(void* state, ::curl_off_t, ::curl_off_t, ::curl_off_t,
                           ::curl_off_t) noexcept
    {
        return static_cast<const std::stop_token*>(state)->stop_requested();
    }

    bool load_weight(const std::string& base_url, std::string_view endpoint,
                     const std::filesystem::path& path, std::stop_token stop)
    {
        static const auto initialized = ::curl_global_init(CURL_GLOBAL_DEFAULT);
        if (stop.stop_requested() || initialized != CURLE_OK || !valid_file(path))
        {
            return false;
        }
        const std::unique_ptr<::CURL, decltype(&::curl_easy_cleanup)> easy(::curl_easy_init(),
                                                                           ::curl_easy_cleanup);
        if (!easy)
        {
            return false;
        }
        const auto filename = path.u8string();
        const std::unique_ptr<char, decltype(&::curl_free)> parameter(
            ::curl_easy_escape(easy.get(), reinterpret_cast<const char*>(filename.data()),
                               static_cast<int>(filename.size())),
            ::curl_free);
        if (!parameter)
        {
            return false;
        }
        const auto url =
            std::string(base_url).append(endpoint).append("?weights_path=").append(parameter.get());
        if (::curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_PROXY, "") != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_PROTOCOLS_STR, "http") != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS, 60'000L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_XFERINFOFUNCTION, cancel_weight_load) !=
                CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_XFERINFODATA, &stop) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, discard_response) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_MAXFILESIZE_LARGE, 8192LL) != CURLE_OK ||
            ::curl_easy_perform(easy.get()) != CURLE_OK)
        {
            return false;
        }
        long status{};
        return ::curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
               status == 200;
    }

    bool load_pair(const std::string& url, const adapters::ModelWeights& weights,
                   std::stop_token stop)
    {
        return load_weight(url, "/set_gpt_weights", weights.gpt, stop) &&
               load_weight(url, "/set_sovits_weights", weights.sovits, stop);
    }
}

export namespace meowlive::adapters
{
    class ManagedModelWeights
    {
    public:
        ManagedModelWeights(std::string url, meowlive::adapters::ModelWeights defaults)
            : url_(std::move(url)), defaults_(std::move(defaults))
        {
        }

        bool configured() const
        {
            return loopback_service(url_) && valid_file(defaults_.gpt) &&
                   valid_file(defaults_.sovits);
        }

        bool select(const std::optional<meowlive::adapters::ModelWeights>& selection,
                    std::stop_token stop)
        {
            const std::lock_guard lock(mutex_);
            if (stop.stop_requested() || !this->configured())
            {
                return false;
            }
            const auto& desired = selection ? *selection : defaults_;
            if (current_ == desired)
            {
                return true;
            }
            if (load_pair(url_, desired, stop))
            {
                current_ = desired;
                return true;
            }
            auto previous = std::exchange(current_, std::nullopt);
            const auto& restore = previous ? *previous : defaults_;
            if (!stop.stop_requested() && load_pair(url_, restore, stop))
            {
                current_ = restore;
            }
            return false;
        }

        void invalidate()
        {
            const std::lock_guard lock(mutex_);
            current_.reset();
        }

    private:
        std::string url_;
        meowlive::adapters::ModelWeights defaults_;
        std::optional<meowlive::adapters::ModelWeights> current_;
        std::mutex mutex_;
    };
}
