module;

#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.speech.http;

import meowlive.standard;
import meowlive.adapters.http.curl_resources;
import meowlive.adapters.http.response_body;

namespace
{
    namespace http = meowlive::adapters::http;

    struct CurlRuntime
    {
        const bool initialized =
            http::check_curl_memory(::curl_global_init(CURL_GLOBAL_DEFAULT)) == CURLE_OK;

        ~CurlRuntime()
        {
            if (initialized)
            {
                ::curl_global_cleanup();
            }
        }
    };

    struct AttachedRequest
    {
        ::CURLM* multi;
        ::CURL* easy;

        ~AttachedRequest()
        {
            ::curl_multi_remove_handle(multi, easy);
        }
    };

    std::expected<std::string, std::string> tts_endpoint(const std::string& base_url)
    {
        constexpr auto invalid =
            "speech engine base URL must be HTTP or HTTPS without credentials, query or fragment";
        if (base_url.empty() || base_url.size() > 4096 ||
            base_url.find_first_of("?#") != std::string::npos ||
            base_url.find('\0') != std::string::npos ||
            (!base_url.starts_with("http://") && !base_url.starts_with("https://")))
        {
            return std::unexpected(invalid);
        }

        const std::unique_ptr<::CURLU, decltype(&::curl_url_cleanup)> url(::curl_url(),
                                                                          ::curl_url_cleanup);
        if (!url)
        {
            throw std::bad_alloc{};
        }
        if (http::check_curl_memory(::curl_url_set(url.get(), CURLUPART_URL, base_url.c_str(),
                                                   CURLU_DISALLOW_USER)) != CURLUE_OK)
        {
            return std::unexpected(invalid);
        }

        return base_url.substr(0, base_url.find_last_not_of('/') + 1) + "/tts";
    }
}

export namespace meowlive::adapters::speech_http
{
    std::expected<std::string, std::string>
    post_tts(const std::string& base_url, const std::string& payload, std::chrono::seconds timeout,
             std::size_t max_audio_bytes, std::stop_token stop)
    {
        constexpr auto client_failed = "could not initialize the speech engine HTTP client";
        constexpr auto request_failed = "speech engine request failed";
        constexpr auto response_too_large = "speech engine audio exceeds the response size limit";
        constexpr auto cancelled = "speech synthesis cancelled";

        if (stop.stop_requested())
        {
            return std::unexpected(cancelled);
        }
        static const CurlRuntime runtime;
        if (!runtime.initialized)
        {
            return std::unexpected(client_failed);
        }
        const auto endpoint = tts_endpoint(base_url);
        if (!endpoint)
        {
            return std::unexpected(endpoint.error());
        }

        http::ResponseBody body(max_audio_bytes);
        std::unique_ptr<::curl_slist, decltype(&::curl_slist_free_all)> headers(
            ::curl_slist_append(nullptr, "Content-Type: application/json"), ::curl_slist_free_all);
        if (!headers || !::curl_slist_append(headers.get(), "Accept: audio/wav"))
        {
            throw std::bad_alloc{};
        }

        const std::unique_ptr<::CURL, decltype(&::curl_easy_cleanup)> easy(::curl_easy_init(),
                                                                           ::curl_easy_cleanup);
        const std::unique_ptr<::CURLM, decltype(&::curl_multi_cleanup)> multi(::curl_multi_init(),
                                                                              ::curl_multi_cleanup);
        if (!easy || !multi)
        {
            throw std::runtime_error("could not initialize libcurl speech request handles");
        }
        const auto deadline_ms = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        if (http::check_curl_memory(
                ::curl_easy_setopt(easy.get(), CURLOPT_URL, endpoint->c_str())) != CURLE_OK ||
            http::check_curl_memory(
                ::curl_easy_setopt(easy.get(), CURLOPT_PROTOCOLS_STR, "http,https")) != CURLE_OK ||
            http::check_curl_memory(::curl_easy_setopt(easy.get(), CURLOPT_PROXY, "")) !=
                CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS, deadline_ms) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_FAILONERROR, 1L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDSIZE,
                               static_cast<long>(payload.size())) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS, payload.data()) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_MAXFILESIZE_LARGE,
                               static_cast<::curl_off_t>(max_audio_bytes)) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, http::ResponseBody::receive) !=
                CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK ||
            http::check_curl_memory(::curl_multi_add_handle(multi.get(), easy.get())) != CURLM_OK)
        {
            throw std::logic_error("libcurl rejected speech request options or handle");
        }
        const AttachedRequest attached{multi.get(), easy.get()};
        const std::stop_callback wake_on_cancel(stop,
                                                [handle = multi.get()]
                                                {
                                                    ::curl_multi_wakeup(handle);
                                                });
        int running{};
        do
        {
            if (stop.stop_requested())
            {
                return std::unexpected(cancelled);
            }
            const auto performed = ::curl_multi_perform(multi.get(), &running);
            body.rethrow_failure();
            if (http::check_curl_memory(performed) != CURLM_OK)
            {
                throw std::runtime_error(std::string("libcurl speech transfer failed: ") +
                                         ::curl_multi_strerror(performed));
            }
            if (running && http::check_curl_memory(::curl_multi_poll(multi.get(), nullptr, 0, 1000,
                                                                     nullptr)) != CURLM_OK)
            {
                return std::unexpected(request_failed);
            }
        } while (running != 0);

        if (stop.stop_requested())
        {
            return std::unexpected(cancelled);
        }
        int remaining{};
        auto* result = ::curl_multi_info_read(multi.get(), &remaining);
        if (!result || result->msg != CURLMSG_DONE)
        {
            throw std::logic_error("libcurl speech request completed without a transfer result");
        }
        const auto result_code = http::check_curl_memory(result->data.result);
        long status{};
        ::curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status != 0 && (status < 200 || status >= 300))
        {
            return std::unexpected("speech engine returned HTTP " + std::to_string(status));
        }
        if (result_code == CURLE_OPERATION_TIMEDOUT)
        {
            return std::unexpected("speech engine request timed out");
        }
        if (body.exceeded() || result_code == CURLE_FILESIZE_EXCEEDED)
        {
            return std::unexpected(response_too_large);
        }
        if (result_code != CURLE_OK)
        {
            return std::unexpected(std::string(request_failed) + ": " +
                                   ::curl_easy_strerror(result_code));
        }
        if (status == 0)
        {
            return std::unexpected(request_failed);
        }
        return body.take_bytes();
    }
}
