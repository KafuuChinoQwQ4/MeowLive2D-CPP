module;

#if defined(_WIN32)
#define NOMINMAX
#endif
#include <curl/curl.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.websocket;

import meowlive.standard;
import meowlive.adapters.bilibili.packet;
import meowlive.adapters.bilibili.transport;
import meowlive.adapters.http.curl_resources;

namespace
{
    namespace bili = meowlive::adapters::bilibili;
    using Clock = std::chrono::steady_clock;
    constexpr auto poll_interval = std::chrono::milliseconds(50);
}

export namespace meowlive::adapters::bilibili
{
    class WebSocket
    {
    public:
        bool connect(const std::string& url, meowlive::adapters::bilibili::TimePoint deadline,
                     std::stop_token stop)
        {
            const auto timeout =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            return timeout.count() > 0 && transfer_.configure(url, "wss,ws", timeout) &&
                   ::curl_easy_setopt(transfer_.easy(), CURLOPT_CONNECT_ONLY, 2L) == CURLE_OK &&
                   transfer_.perform(stop) == CURLE_OK &&
                   ::curl_easy_getinfo(transfer_.easy(), CURLINFO_ACTIVESOCKET, &socket_) ==
                       CURLE_OK &&
                   socket_ != CURL_SOCKET_BAD;
        }

        bool send(std::string_view message, meowlive::adapters::bilibili::TimePoint deadline,
                  std::stop_token stop)
        {
            auto fragment_size = static_cast<curl_off_t>(message.size());
            while (!stop.stop_requested() && Clock::now() < deadline)
            {
                std::size_t sent{};
                const auto status = meowlive::adapters::http::check_curl_memory(
                    ::curl_ws_send(transfer_.easy(), message.data(), message.size(), &sent,
                                   fragment_size, CURLWS_BINARY | CURLWS_OFFSET));
                if (status == CURLE_AGAIN)
                {
                    if (!wait(CURL_WAIT_POLLOUT, deadline))
                    {
                        return false;
                    }
                    continue;
                }
                if (status != CURLE_OK || sent > message.size())
                {
                    return false;
                }
                message.remove_prefix(sent);
                if (message.empty())
                {
                    return true;
                }
                fragment_size = 0;
            }
            return false;
        }

        // Empty optional means no complete binary message is currently available.
        std::expected<std::optional<std::string>, ::CURLcode> receive()
        {
            std::size_t count{};
            const ::curl_ws_frame* meta{};
            const auto status = meowlive::adapters::http::check_curl_memory(
                ::curl_ws_recv(transfer_.easy(), chunk_.data(), chunk_.size(), &count, &meta));
            if (status == CURLE_AGAIN)
            {
                return std::nullopt;
            }
            if (status != CURLE_OK || !meta || (meta->flags & CURLWS_CLOSE))
            {
                return std::unexpected(status == CURLE_OK ? CURLE_GOT_NOTHING : status);
            }
            const bool binary = (meta->flags & CURLWS_BINARY) != 0;
            if (!binary && (meta->flags & CURLWS_TEXT) == 0)
            {
                return std::nullopt;
            }
            if (meta->bytesleft < 0 || count > bili::max_session_frame - pending_.size() ||
                meta->bytesleft >
                    static_cast<curl_off_t>(bili::max_session_frame - pending_.size() - count))
            {
                return std::unexpected(CURLE_TOO_LARGE);
            }
            pending_.append(chunk_.data(), count);
            if (meta->bytesleft || (meta->flags & CURLWS_CONT))
            {
                return std::nullopt;
            }
            if (!binary)
            {
                pending_.clear();
                return std::nullopt;
            }
            auto message = std::move(pending_);
            pending_.clear();
            return message;
        }

        bool wait_readable(meowlive::adapters::bilibili::TimePoint deadline)
        {
            return wait(CURL_WAIT_POLLIN, deadline);
        }

        void close()
        {
            std::size_t sent{};
            ::curl_ws_send(transfer_.easy(), nullptr, 0, &sent, 0, CURLWS_CLOSE);
        }

    private:
        bool wait(short events, meowlive::adapters::bilibili::TimePoint deadline)
        {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0)
            {
                return true;
            }
            ::curl_waitfd socket{socket_, events, 0};
            const auto timeout = static_cast<int>(std::min(remaining, poll_interval).count());
            return meowlive::adapters::http::check_curl_memory(::curl_multi_poll(
                       transfer_.multi(), &socket, 1, timeout, nullptr)) == CURLM_OK;
        }

        meowlive::adapters::bilibili::Transfer transfer_;
        curl_socket_t socket_ = CURL_SOCKET_BAD;
        std::string pending_;
        std::array<char, 8192> chunk_{};
    };
}
