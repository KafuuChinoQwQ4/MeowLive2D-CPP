module;

#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.llm.http;

import meowlive.standard;
import meowlive.adapters.http.curl_resources;
import meowlive.adapters.http.response_body;
import meowlive.application.llm;

export namespace meowlive::adapters::llm_detail
{
    struct HttpResponse
    {
        std::string body;
        std::string content_type;
        std::optional<std::uint64_t> first_response_ms;
    };
}

namespace
{
    namespace llm = meowlive::adapters::llm_detail;
    namespace app = meowlive::application;
    namespace http = meowlive::adapters::http;

    struct Attachment
    {
        ::CURLM* multi;
        ::CURL* easy;
        ~Attachment()
        {
            ::curl_multi_remove_handle(multi, easy);
        }
    };
}

export namespace meowlive::adapters::llm_detail
{
    std::expected<std::string, app::ModelError> validate_model_url(const std::string& input)
    {
        const app::ModelError invalid{
            "模型服务地址须为无凭据、查询参数和片段的 HTTP 或 HTTPS 地址"};
        if (input.empty() || input.size() > 4096 ||
            (!input.starts_with("http://") && !input.starts_with("https://")) ||
            input.find_first_of("?#\\") != std::string::npos ||
            std::ranges::any_of(input,
                                [](unsigned char ch)
                                {
                                    return ch <= 32 || ch == 127;
                                }))
        {
            return std::unexpected(invalid);
        }

        const std::unique_ptr<::CURLU, decltype(&::curl_url_cleanup)> parsed(::curl_url(),
                                                                             ::curl_url_cleanup);
        if (!parsed)
        {
            throw std::bad_alloc{};
        }
        if (http::check_curl_memory(::curl_url_set(parsed.get(), CURLUPART_URL, input.c_str(),
                                                   CURLU_DISALLOW_USER)) != CURLUE_OK)
        {
            return std::unexpected(invalid);
        }
        return input.substr(0, input.find_last_not_of('/') + 1);
    }

    std::expected<llm::HttpResponse, app::ModelError>
    model_http(const std::string& url, std::span<const std::string> headers,
               const std::string* payload, std::chrono::seconds timeout, std::size_t limit,
               std::stop_token stop)
    {
        const app::ModelError cancelled{"模型请求已取消"};
        const app::ModelError transport{"模型服务连接失败", true};
        if (stop.stop_requested())
        {
            return std::unexpected(cancelled);
        }
        // Keep libcurl initialized for the process so concurrent adapters may use it.
        static const auto initialized =
            http::check_curl_memory(::curl_global_init(CURL_GLOBAL_DEFAULT));
        if (initialized != CURLE_OK)
        {
            return std::unexpected(transport);
        }

        std::unique_ptr<::curl_slist, decltype(&::curl_slist_free_all)> list(nullptr,
                                                                             ::curl_slist_free_all);
        for (const auto& header : headers)
        {
            auto* appended = ::curl_slist_append(list.get(), header.c_str());
            if (!appended)
            {
                throw std::bad_alloc{};
            }
            list.release();
            list.reset(appended);
        }

        http::ResponseBody body(limit);
        const std::unique_ptr<::CURL, decltype(&::curl_easy_cleanup)> easy(::curl_easy_init(),
                                                                           ::curl_easy_cleanup);
        const std::unique_ptr<::CURLM, decltype(&::curl_multi_cleanup)> multi(::curl_multi_init(),
                                                                              ::curl_multi_cleanup);
        if (!easy || !multi)
        {
            throw std::runtime_error("could not initialize libcurl model request handles");
        }
        const auto timeout_ms = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        if (http::check_curl_memory(::curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str())) !=
                CURLE_OK ||
            http::check_curl_memory(
                ::curl_easy_setopt(easy.get(), CURLOPT_PROTOCOLS_STR, "http,https")) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS, timeout_ms) != CURLE_OK ||
            http::check_curl_memory(::curl_easy_setopt(easy.get(), CURLOPT_PROXY, "")) !=
                CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, list.get()) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_FAILONERROR, 1L) != CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, http::ResponseBody::receive) !=
                CURLE_OK ||
            ::curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK)
        {
            throw std::logic_error("libcurl rejected model request options");
        }
        if (payload &&
            (::curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDSIZE,
                                static_cast<long>(payload->size())) != CURLE_OK ||
             ::curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS, payload->data()) != CURLE_OK))
        {
            throw std::logic_error("libcurl rejected model request payload options");
        }
        if (http::check_curl_memory(::curl_multi_add_handle(multi.get(), easy.get())) != CURLM_OK)
        {
            throw std::logic_error("libcurl rejected the model request handle");
        }
        const Attachment attachment{multi.get(), easy.get()};
        const auto started = std::chrono::steady_clock::now();
        const std::stop_callback wake(stop,
                                      [handle = multi.get()]
                                      {
                                          ::curl_multi_wakeup(handle);
                                      });
        int active{};
        do
        {
            if (stop.stop_requested())
            {
                return std::unexpected(cancelled);
            }
            const auto performed = ::curl_multi_perform(multi.get(), &active);
            body.rethrow_failure();
            if (http::check_curl_memory(performed) != CURLM_OK)
            {
                throw std::runtime_error(std::string("libcurl model transfer failed: ") +
                                         ::curl_multi_strerror(performed));
            }
            if (active && http::check_curl_memory(::curl_multi_poll(multi.get(), nullptr, 0, 1000,
                                                                    nullptr)) != CURLM_OK)
            {
                return std::unexpected(transport);
            }
        } while (active);
        if (stop.stop_requested())
        {
            return std::unexpected(cancelled);
        }

        int remaining{};
        const auto* result = ::curl_multi_info_read(multi.get(), &remaining);
        if (!result || result->msg != CURLMSG_DONE)
        {
            throw std::logic_error("libcurl model request completed without a transfer result");
        }
        const auto result_code = http::check_curl_memory(result->data.result);
        long status{};
        ::curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status && (status < 200 || status >= 300))
        {
            return std::unexpected(
                app::ModelError{"模型服务返回 HTTP " + std::to_string(status),
                                status == 408 || status == 425 || status == 429 || status >= 500});
        }
        if (result_code == CURLE_OPERATION_TIMEDOUT)
        {
            return std::unexpected(app::ModelError{"模型请求超时", true});
        }
        if (body.exceeded())
        {
            return std::unexpected(app::ModelError{"模型响应超过大小上限"});
        }
        if (result_code != CURLE_OK)
        {
            return std::unexpected(app::ModelError{
                std::string("模型服务连接失败：") + ::curl_easy_strerror(result_code), true});
        }
        if (!status)
        {
            return std::unexpected(transport);
        }
        char* content_type{};
        ::curl_easy_getinfo(easy.get(), CURLINFO_CONTENT_TYPE, &content_type);
        std::optional<std::uint64_t> first_response_ms;
        if (const auto received = body.first_received())
        {
            first_response_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(*received - started).count());
        }
        return llm::HttpResponse{body.take_bytes(), content_type ? content_type : "",
                                 first_response_ms};
    }
}
