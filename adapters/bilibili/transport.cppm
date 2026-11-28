module;

#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.transport;

import meowlive.standard;
import meowlive.adapters.http.curl_resources;

export namespace meowlive::adapters::bilibili
{
    using TimePoint = std::chrono::steady_clock::time_point;

    bool safe_endpoint(const std::string& url, std::string_view secure_scheme,
                       std::string_view loopback_scheme, bool allow_loopback)
    {
        if (url.empty() || url.size() > 4096 ||
            url.find_first_of("\r\n\t \\#") != std::string::npos)
        {
            return false;
        }
        const std::unique_ptr<::CURLU, decltype(&::curl_url_cleanup)> parsed(::curl_url(),
                                                                             ::curl_url_cleanup);
        if (!parsed)
        {
            throw std::bad_alloc{};
        }
        if (meowlive::adapters::http::check_curl_memory(
                ::curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(),
                               CURLU_DISALLOW_USER | CURLU_NON_SUPPORT_SCHEME)) != CURLUE_OK)
        {
            return false;
        }
        char* scheme_text{};
        if (meowlive::adapters::http::check_curl_memory(
                ::curl_url_get(parsed.get(), CURLUPART_SCHEME, &scheme_text, 0)) != CURLUE_OK)
        {
            return false;
        }
        const std::unique_ptr<char, decltype(&::curl_free)> scheme(scheme_text, ::curl_free);
        if (scheme.get() == secure_scheme)
        {
            return true;
        }
        if (!allow_loopback || scheme.get() != loopback_scheme)
        {
            return false;
        }
        char* host_text{};
        if (meowlive::adapters::http::check_curl_memory(
                ::curl_url_get(parsed.get(), CURLUPART_HOST, &host_text, 0)) != CURLUE_OK)
        {
            return false;
        }
        const std::unique_ptr<char, decltype(&::curl_free)> host(host_text, ::curl_free);
        const std::string_view hostname = host.get();
        return hostname == "127.0.0.1" || hostname == "[::1]";
    }

    class Transfer
    {
    public:
        Transfer()
        {
            static const auto initialized = meowlive::adapters::http::check_curl_memory(
                ::curl_global_init(CURL_GLOBAL_DEFAULT));
            if (initialized == CURLE_OK)
            {
                easy_.reset(::curl_easy_init());
                multi_.reset(::curl_multi_init());
            }
        }

        ~Transfer()
        {
            if (attached_)
            {
                ::curl_multi_remove_handle(multi_.get(), easy_.get());
            }
        }

        ::CURL* easy() const
        {
            return easy_.get();
        }

        ::CURLM* multi() const
        {
            return multi_.get();
        }

        bool configure(const std::string& url, const char* protocols,
                       std::chrono::milliseconds timeout)
        {
            const auto timeout_ms = static_cast<long>(timeout.count());
            return easy_ && multi_ &&
                   meowlive::adapters::http::check_curl_memory(
                       ::curl_easy_setopt(easy_.get(), CURLOPT_URL, url.c_str())) == CURLE_OK &&
                   meowlive::adapters::http::check_curl_memory(::curl_easy_setopt(
                       easy_.get(), CURLOPT_PROTOCOLS_STR, protocols)) == CURLE_OK &&
                   ::curl_easy_setopt(easy_.get(), CURLOPT_TIMEOUT_MS, timeout_ms) == CURLE_OK &&
                   ::curl_easy_setopt(easy_.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                   ::curl_easy_setopt(easy_.get(), CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
                   meowlive::adapters::http::check_curl_memory(
                       ::curl_easy_setopt(easy_.get(), CURLOPT_PROXY, "")) == CURLE_OK &&
                   ::curl_easy_setopt(easy_.get(), CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
                   ::curl_easy_setopt(easy_.get(), CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK;
        }

        ::CURLcode perform(std::stop_token stop)
        {
            if (meowlive::adapters::http::check_curl_memory(
                    ::curl_multi_add_handle(multi_.get(), easy_.get())) != CURLM_OK)
            {
                throw std::logic_error("libcurl rejected the Bilibili request handle");
            }
            attached_ = true;
            const std::stop_callback wake(stop,
                                          [handle = multi_.get()]
                                          {
                                              ::curl_multi_wakeup(handle);
                                          });
            int active{};
            do
            {
                if (stop.stop_requested())
                {
                    return CURLE_ABORTED_BY_CALLBACK;
                }
                const auto performed = meowlive::adapters::http::check_curl_memory(
                    ::curl_multi_perform(multi_.get(), &active));
                if (performed != CURLM_OK)
                {
                    throw std::runtime_error(std::string("libcurl Bilibili transfer failed: ") +
                                             ::curl_multi_strerror(performed));
                }
                if (active && meowlive::adapters::http::check_curl_memory(::curl_multi_poll(
                                  multi_.get(), nullptr, 0, 250, nullptr)) != CURLM_OK)
                {
                    return CURLE_RECV_ERROR;
                }
            } while (active);

            int remaining{};
            const auto* result = ::curl_multi_info_read(multi_.get(), &remaining);
            if (!result || result->msg != CURLMSG_DONE)
            {
                throw std::logic_error(
                    "libcurl Bilibili request completed without a transfer result");
            }
            return meowlive::adapters::http::check_curl_memory(result->data.result);
        }

    private:
        std::unique_ptr<::CURL, decltype(&::curl_easy_cleanup)> easy_{nullptr, ::curl_easy_cleanup};
        std::unique_ptr<::CURLM, decltype(&::curl_multi_cleanup)> multi_{nullptr,
                                                                         ::curl_multi_cleanup};
        bool attached_{};
    };
}
