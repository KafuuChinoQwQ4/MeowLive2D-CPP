module;

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <drogon/utils/Utilities.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.tests.loopback_socket;

import meowlive.standard;

export namespace meowlive::tests::transport_detail
{
    inline constexpr std::size_t message_limit = 1024 * 1024;

#if defined(_WIN32)
    using SocketHandle = SOCKET;
    inline constexpr SocketHandle invalid_socket = INVALID_SOCKET;

    class Winsock
    {
    public:
        Winsock()
        {
            WSADATA data{};
            if (const int error = ::WSAStartup(MAKEWORD(2, 2), &data); error != 0)
            {
                throw std::system_error(error, std::system_category(), "WSAStartup");
            }
        }

        ~Winsock()
        {
            ::WSACleanup();
        }
    };
#else
    using SocketHandle = int;
    inline constexpr SocketHandle invalid_socket = -1;
#endif

    [[noreturn]] void socket_error(std::string_view action)
    {
#if defined(_WIN32)
        const int error = ::WSAGetLastError();
        throw std::system_error(error, std::system_category(), std::string(action));
#else
        const int error = errno;
        throw std::system_error(error, std::generic_category(), std::string(action));
#endif
    }

    struct EndOfStream : std::runtime_error
    {
        EndOfStream() : std::runtime_error("Unexpected end of loopback connection")
        {
        }
    };

    class LoopbackSocket
    {
    public:
        LoopbackSocket()
        {
#if defined(_WIN32)
            static const Winsock winsock;
#endif
            handle_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (handle_ == invalid_socket)
            {
                socket_error("create loopback socket");
            }
        }

        ~LoopbackSocket()
        {
#if defined(_WIN32)
            ::closesocket(handle_);
#else
            ::close(handle_);
#endif
        }

        LoopbackSocket(const LoopbackSocket&) = delete;
        LoopbackSocket& operator=(const LoopbackSocket&) = delete;

        void connect(unsigned short port, int timeout_seconds)
        {
#if defined(_WIN32)
            const DWORD timeout_ms = static_cast<DWORD>(timeout_seconds) * 1000;
            const auto* timeout = reinterpret_cast<const char*>(&timeout_ms);
            constexpr auto timeout_size = sizeof(timeout_ms);
#else
            const timeval timeout_value{timeout_seconds, 0};
            const auto* timeout = &timeout_value;
            constexpr auto timeout_size = sizeof(timeout_value);
#endif
            if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, timeout, timeout_size) != 0 ||
                ::setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, timeout, timeout_size) != 0)
            {
                socket_error("configure loopback socket");
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = ::htons(port);
            address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            if (::connect(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
                0)
            {
                socket_error("connect to loopback service");
            }
        }

        void write(std::string_view data)
        {
            while (!data.empty())
            {
#if defined(_WIN32)
                const auto written = ::send(handle_, data.data(), static_cast<int>(data.size()), 0);
#else
                const auto written = ::send(handle_, data.data(), data.size(), MSG_NOSIGNAL);
                if (written < 0 && errno == EINTR)
                {
                    continue;
                }
#endif
                if (written < 0)
                {
                    socket_error("send to loopback service");
                }
                if (written == 0)
                {
                    throw EndOfStream();
                }
                data.remove_prefix(static_cast<std::size_t>(written));
            }
        }

        std::string read(std::size_t size)
        {
            if (size > message_limit)
            {
                throw std::runtime_error("Loopback response exceeds test limit");
            }
            while (pending_.size() < size)
            {
                if (!receive())
                {
                    throw EndOfStream();
                }
            }
            return consume(size);
        }

        std::string read_line()
        {
            std::size_t end = pending_.find("\r\n");
            while (end == std::string::npos)
            {
                if (!receive())
                {
                    throw EndOfStream();
                }
                end = pending_.find("\r\n");
            }
            auto line = consume(end + 2);
            line.resize(end);
            return line;
        }

        std::string read_to_close()
        {
            while (receive())
            {
            }
            return std::move(pending_);
        }

    private:
        bool receive()
        {
            std::array<char, 4096> buffer{};
            while (true)
            {
#if defined(_WIN32)
                const auto count =
                    ::recv(handle_, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
                const auto count = ::recv(handle_, buffer.data(), buffer.size(), 0);
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
#endif
                if (count < 0)
                {
                    socket_error("read from loopback service");
                }
                if (count == 0)
                {
                    return false;
                }
                pending_.append(buffer.data(), static_cast<std::size_t>(count));
                if (pending_.size() > message_limit)
                {
                    throw std::runtime_error("Loopback response exceeds test limit");
                }
                return true;
            }
        }

        std::string consume(std::size_t count)
        {
            auto result = pending_.substr(0, count);
            pending_.erase(0, count);
            return result;
        }

        SocketHandle handle_{};
        std::string pending_;
    };
}
