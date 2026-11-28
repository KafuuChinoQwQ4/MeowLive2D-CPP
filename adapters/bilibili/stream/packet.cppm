module;

#include <brotli/decode.h>
#include <json/json.h>
#include <zlib.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.bilibili.packet;

import meowlive.standard;
import meowlive.adapters.bilibili.notice;
import meowlive.domain.live_event;
import meowlive.protocol.json;

export namespace meowlive::adapters::bilibili
{
    inline constexpr std::size_t max_session_frame = 256 * 1024;

    struct Frame
    {
        std::vector<meowlive::domain::LiveEvent> events;
        std::optional<bool> authorized;
        bool heartbeat{};
        bool interaction_ended{};
    };
}

namespace
{
    namespace bili = meowlive::adapters::bilibili;
    constexpr std::size_t header_size = 16;
    constexpr std::size_t max_inflated_bytes = 1024 * 1024;
    constexpr std::size_t max_packets = 256;
    constexpr unsigned max_depth = 2;

    std::uint32_t read_integer(std::string_view bytes)
    {
        std::uint32_t result{};
        for (const auto byte : bytes)
        {
            result = (result << 8) | static_cast<unsigned char>(byte);
        }
        return result;
    }

    void append_integer(std::string& bytes, std::uint32_t value, unsigned length)
    {
        for (unsigned position = length; position > 0; --position)
        {
            bytes.push_back(static_cast<char>((value >> (8 * (position - 1))) & 0xff));
        }
    }

    std::optional<std::string> expand(std::string_view data, std::uint32_t version,
                                      std::size_t remaining_bytes)
    {
        std::string result(remaining_bytes + 1, '\0');
        if (version == 2)
        {
            auto input_size = static_cast<uLong>(data.size());
            auto output_size = static_cast<uLongf>(result.size());
            if (::uncompress2(reinterpret_cast<Bytef*>(result.data()), &output_size,
                              reinterpret_cast<const Bytef*>(data.data()), &input_size) != Z_OK ||
                input_size != data.size() || output_size > remaining_bytes)
            {
                return std::nullopt;
            }
            result.resize(output_size);
        }
        else
        {
            auto output_size = result.size();
            if (::BrotliDecoderDecompress(
                    data.size(), reinterpret_cast<const std::uint8_t*>(data.data()), &output_size,
                    reinterpret_cast<std::uint8_t*>(result.data())) !=
                    BROTLI_DECODER_RESULT_SUCCESS ||
                output_size > remaining_bytes)
            {
                return std::nullopt;
            }
            result.resize(output_size);
        }
        return result;
    }

    struct Decoder
    {
        std::uint64_t room;
        std::uint64_t app_id;
        std::string_view game_id;
        std::size_t packets{};
        std::size_t remaining_bytes = max_inflated_bytes;
        bili::Frame frame;

        bool decode_message(std::string_view data, unsigned depth)
        {
            while (!data.empty())
            {
                if (data.size() < header_size || ++packets > max_packets)
                {
                    return false;
                }
                const auto length = read_integer(data.substr(0, 4));
                const auto header = read_integer(data.substr(4, 2));
                const auto version = read_integer(data.substr(6, 2));
                const auto operation = read_integer(data.substr(8, 4));
                if (header < header_size || length < header || length > data.size())
                {
                    return false;
                }
                if (!decode_packet(data.substr(header, length - header), version, operation, depth))
                {
                    return false;
                }
                data.remove_prefix(length);
            }
            return true;
        }

        bool decode_packet(std::string_view body, std::uint32_t version, std::uint32_t operation,
                           unsigned depth)
        {
            if (version == 2 || version == 3)
            {
                if (depth >= max_depth)
                {
                    return false;
                }
                auto expanded = expand(body, version, remaining_bytes);
                if (!expanded || expanded->empty())
                {
                    return false;
                }
                remaining_bytes -= expanded->size();
                return decode_message(*expanded, depth + 1);
            }
            if (version > 1)
            {
                return false;
            }
            if (operation == 8)
            {
                const auto authority = meowlive::protocol::json::parse_object(body);
                if (!body.empty() && !authority)
                {
                    return false;
                }
                const auto accepted = !authority || !authority->isMember("code") ||
                                      ((*authority)["code"].type() == Json::intValue &&
                                       (*authority)["code"].asInt64() == 0);
                frame.authorized = frame.authorized.value_or(true) && accepted;
            }
            else if (operation == 5 && !frame.interaction_ended)
            {
                auto notice = bili::decode_notice(body, room, app_id, game_id);
                if (notice.event)
                {
                    frame.events.push_back(std::move(*notice.event));
                }
                frame.interaction_ended = notice.interaction_ended;
            }
            else if (operation == 3)
            {
                frame.heartbeat = true;
            }
            return true;
        }
    };
}

export namespace meowlive::adapters::bilibili
{
    std::string encode_packet(std::uint32_t operation, std::string_view body = {},
                              std::uint16_t version = 1)
    {
        if (body.size() > meowlive::adapters::bilibili::max_session_frame - header_size)
        {
            return {};
        }
        std::string packet;
        packet.reserve(header_size + body.size());
        append_integer(packet, static_cast<std::uint32_t>(header_size + body.size()), 4);
        append_integer(packet, header_size, 2);
        append_integer(packet, version, 2);
        append_integer(packet, operation, 4);
        append_integer(packet, 1, 4);
        packet.append(body);
        return packet;
    }

    std::optional<meowlive::adapters::bilibili::Frame> decode_frame(std::string_view bytes,
                                                                    std::uint64_t room,
                                                                    std::uint64_t app_id,
                                                                    std::string_view game_id)
    {
        if (bytes.empty() || bytes.size() > meowlive::adapters::bilibili::max_session_frame)
        {
            return std::nullopt;
        }
        Decoder decoder{room, app_id, game_id};
        if (!decoder.decode_message(bytes, 0))
        {
            return std::nullopt;
        }
        return std::move(decoder.frame);
    }
}
