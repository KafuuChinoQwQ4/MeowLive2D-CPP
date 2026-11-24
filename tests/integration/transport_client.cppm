module;

#if defined(_WIN32)
#include <winsock2.h>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <drogon/utils/Utilities.h>

export module meowlive.tests.transport_client;

#if !defined(_WIN32)
import std;
#endif

namespace meowlive::tests::transport_detail
{
    constexpr std::size_t message_limit = 1024 * 1024;

#if defined(_WIN32)
    using SocketHandle = SOCKET;
    constexpr SocketHandle invalid_socket = INVALID_SOCKET;

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
    constexpr SocketHandle invalid_socket = -1;
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

    std::string lowercase(std::string_view text)
    {
        std::string result(text);
        for (auto& character : result)
        {
            if (character >= 'A' && character <= 'Z')
            {
                character += 'a' - 'A';
            }
        }
        return result;
    }

    std::string_view trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
        {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }

    template <typename Int> Int parse_number(std::string_view value, int base = 10)
    {
        Int number{};
        const auto result =
            std::from_chars(value.data(), value.data() + value.size(), number, base);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        {
            throw std::runtime_error("Invalid numeric value in loopback response");
        }
        return number;
    }
}

export namespace meowlive::tests
{
    struct HttpResponse
    {
        int status{};
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;

        std::string header(std::string_view name) const
        {
            const auto normalized = meowlive::tests::transport_detail::lowercase(name);
            for (const auto& [key, value] : headers)
            {
                if (key == normalized)
                {
                    return value;
                }
            }
            return {};
        }
    };
}

namespace meowlive::tests::transport_detail
{
    std::string read_chunked_body(LoopbackSocket& socket)
    {
        std::string body;
        while (true)
        {
            const auto line = socket.read_line();
            const auto length =
                parse_number<std::size_t>(std::string_view(line).substr(0, line.find(';')), 16);
            if (length == 0)
            {
                while (!socket.read_line().empty())
                {
                }
                return body;
            }
            if (length > message_limit - body.size())
            {
                throw std::runtime_error("Loopback HTTP body exceeds test limit");
            }
            body += socket.read(length);
            if (socket.read(2) != "\r\n")
            {
                throw std::runtime_error("Invalid chunk terminator in loopback HTTP response");
            }
        }
    }

    meowlive::tests::HttpResponse read_response(LoopbackSocket& socket)
    {
        const auto status_line = socket.read_line();
        if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12 || status_line[8] != ' ')
        {
            throw std::runtime_error("Invalid loopback HTTP status line");
        }

        meowlive::tests::HttpResponse response;
        response.status = parse_number<int>(std::string_view(status_line).substr(9, 3));
        std::size_t header_size = status_line.size();
        for (auto line = socket.read_line(); !line.empty(); line = socket.read_line())
        {
            const auto delimiter = line.find(':');
            header_size += line.size() + 2;
            if (delimiter == std::string::npos || header_size > message_limit)
            {
                throw std::runtime_error("Invalid loopback HTTP response headers");
            }
            response.headers.emplace_back(lowercase(std::string_view(line).substr(0, delimiter)),
                                          trim(std::string_view(line).substr(delimiter + 1)));
        }

        if (response.status == 101 || response.status == 204 || response.status == 304)
        {
            return response;
        }
        if (lowercase(response.header("Transfer-Encoding")) == "chunked")
        {
            response.body = read_chunked_body(socket);
        }
        else if (const auto length = response.header("Content-Length"); !length.empty())
        {
            response.body = socket.read(parse_number<std::size_t>(length));
        }
        else
        {
            response.body = socket.read_to_close();
        }
        return response;
    }

    std::string request_headers(unsigned short port, std::string_view path, std::string_view method,
                                std::string_view token, std::string_view origin)
    {
        std::string headers(method);
        headers += ' ';
        headers += path;
        headers += " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
        if (!token.empty())
        {
            headers += "Authorization: Bearer ";
            headers += token;
            headers += "\r\n";
        }
        if (!origin.empty())
        {
            headers += "Origin: ";
            headers += origin;
            headers += "\r\n";
        }
        return headers;
    }

    template <std::size_t Size> std::array<unsigned char, Size> random_bytes()
    {
        std::array<unsigned char, Size> bytes{};
        if (!drogon::utils::secureRandomBytes(bytes.data(), bytes.size()))
        {
            throw std::runtime_error("Cannot generate loopback WebSocket handshake or frame data");
        }
        return bytes;
    }

    std::string expected_accept(std::string key)
    {
        key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const auto hash = trantor::utils::sha1(key);
        return drogon::utils::base64Encode(reinterpret_cast<const unsigned char*>(&hash),
                                           sizeof(hash));
    }

    std::uint64_t frame_length(std::string_view bytes)
    {
        std::uint64_t length{};
        for (const unsigned char value : bytes)
        {
            length = (length << 8) | value;
        }
        return length;
    }
}

export namespace meowlive::tests
{
    meowlive::tests::HttpResponse request(unsigned short port, std::string_view path,
                                          std::string_view method = "GET",
                                          std::string_view token = {},
                                          std::optional<std::string_view> body = {},
                                          std::string_view origin = {})
    {
        meowlive::tests::transport_detail::LoopbackSocket socket;
        socket.connect(port, 3);
        auto headers =
            meowlive::tests::transport_detail::request_headers(port, path, method, token, origin);
        headers += "Connection: close\r\n";
        if (body)
        {
            headers += "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(body->size()) + "\r\n";
        }
        headers += "\r\n";
        socket.write(headers);
        if (body)
        {
            socket.write(*body);
        }
        return meowlive::tests::transport_detail::read_response(socket);
    }

    class WebSocket
    {
    public:
        WebSocket(unsigned short port, std::string_view path, std::string_view token = {},
                  std::string_view origin = {})
        {
            socket_.connect(port, 8);
            const auto nonce = meowlive::tests::transport_detail::random_bytes<16>();
            const auto key = drogon::utils::base64Encode(nonce.data(), nonce.size());
            auto headers = meowlive::tests::transport_detail::request_headers(port, path, "GET",
                                                                              token, origin);
            headers += "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n";
            headers += "Sec-WebSocket-Key: " + key + "\r\n\r\n";
            socket_.write(headers);
            response_ = meowlive::tests::transport_detail::read_response(socket_);
            if (response_.status == 101 &&
                response_.header("Sec-WebSocket-Accept") !=
                    meowlive::tests::transport_detail::expected_accept(key))
            {
                throw std::runtime_error("Invalid Sec-WebSocket-Accept in loopback upgrade");
            }
        }

        const meowlive::tests::HttpResponse& response() const
        {
            return response_;
        }

        void send(unsigned opcode, std::string_view payload)
        {
            if (response_.status != 101 || payload.size() > 0xFFFF || opcode > 0xF ||
                ((opcode & 0x8) != 0 && payload.size() > 125))
            {
                throw std::runtime_error("Cannot send loopback WebSocket frame");
            }
            const auto mask = meowlive::tests::transport_detail::random_bytes<4>();
            std::string frame(1, static_cast<char>(0x80 | opcode));
            if (payload.size() < 126)
            {
                frame += static_cast<char>(0x80 | payload.size());
            }
            else
            {
                frame += static_cast<char>(0x80 | 126);
                frame += static_cast<char>(payload.size() >> 8);
                frame += static_cast<char>(payload.size() & 0xFF);
            }
            frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
            for (std::size_t index = 0; index < payload.size(); ++index)
            {
                frame += static_cast<char>(static_cast<unsigned char>(payload[index]) ^
                                           mask[index % mask.size()]);
            }
            socket_.write(frame);
        }

        std::string wait_frame(unsigned expected_opcode)
        {
            while (true)
            {
                const auto header = socket_.read(2);
                const auto flags = static_cast<unsigned char>(header[0]);
                const auto size_byte = static_cast<unsigned char>(header[1]);
                if ((flags & 0xF0) != 0x80 || (size_byte & 0x80) != 0)
                {
                    throw std::runtime_error("Invalid WebSocket frame flags in loopback response");
                }
                const auto opcode = flags & 0xF;
                std::uint64_t length = size_byte;
                if (length >= 126)
                {
                    length = meowlive::tests::transport_detail::frame_length(
                        socket_.read(length == 126 ? 2 : 8));
                }
                if (length > meowlive::tests::transport_detail::message_limit ||
                    ((opcode & 0x8) != 0 && length > 125))
                {
                    throw std::runtime_error("Invalid WebSocket frame length in loopback response");
                }
                auto payload = socket_.read(static_cast<std::size_t>(length));
                if (opcode == expected_opcode)
                {
                    return payload;
                }
                if (opcode != 9)
                {
                    throw std::runtime_error("Unexpected loopback WebSocket opcode " +
                                             std::to_string(opcode));
                }
                send(10, payload);
            }
        }

        void wait_closed()
        {
            try
            {
                wait_frame(8);
            }
            catch (const meowlive::tests::transport_detail::EndOfStream&)
            {
            }
        }

    private:
        meowlive::tests::transport_detail::LoopbackSocket socket_;
        meowlive::tests::HttpResponse response_;
    };
}
