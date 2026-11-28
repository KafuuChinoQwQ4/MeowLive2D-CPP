module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.audio;

import meowlive.standard;
import meowlive.protocol.utf8;
import meowlive.protocol.version;

export namespace meowlive::protocol
{
    inline constexpr std::string_view audio_magic{"MLPC"};
    inline constexpr std::size_t audio_header_bytes{22};
    inline constexpr std::size_t max_utterance_id_bytes{128};
    inline constexpr std::size_t max_audio_samples{16'384};
    inline constexpr std::size_t max_audio_frame_bytes{meowlive::protocol::audio_header_bytes +
                                                       meowlive::protocol::max_utterance_id_bytes +
                                                       meowlive::protocol::max_audio_samples * 2};

    struct AudioFormat
    {
        std::uint32_t sample_rate{};
        std::uint16_t channels{};

        bool operator==(const meowlive::protocol::AudioFormat&) const = default;
    };

    bool valid_audio_format(const meowlive::protocol::AudioFormat format)
    {
        return format.sample_rate >= 8'000 && format.sample_rate <= 96'000 &&
               format.channels >= 1 && format.channels <= 2;
    }

    struct AudioChunk
    {
        std::string utterance_id;
        std::uint32_t generation{};
        std::uint32_t sequence{};
        std::vector<std::int16_t> samples;
        bool end{};

        bool operator==(const meowlive::protocol::AudioChunk&) const = default;
    };
}

namespace
{
    template <std::unsigned_integral Integer>
    void append_little_endian(std::vector<std::uint8_t>& frame, const Integer value)
    {
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
        {
            frame.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
        }
    }

    template <std::unsigned_integral Integer>
    Integer read_little_endian(const std::span<const std::uint8_t> frame, const std::size_t offset)
    {
        std::uint32_t value{};
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
        {
            value |= static_cast<std::uint32_t>(frame[offset + byte]) << (byte * 8);
        }
        return static_cast<Integer>(value);
    }
}

export namespace meowlive::protocol
{
    std::expected<std::vector<std::uint8_t>, std::string>
    encode_audio_chunk(const meowlive::protocol::AudioChunk& chunk)
    {
        if (chunk.utterance_id.empty() ||
            chunk.utterance_id.size() > meowlive::protocol::max_utterance_id_bytes ||
            chunk.samples.size() > meowlive::protocol::max_audio_samples)
        {
            return std::unexpected("audio identity or payload exceeds frame bounds");
        }
        if (!meowlive::protocol::is_utf8(chunk.utterance_id))
        {
            return std::unexpected("audio identity is not UTF-8");
        }

        std::vector<std::uint8_t> frame;
        frame.reserve(meowlive::protocol::audio_header_bytes + chunk.utterance_id.size() +
                      chunk.samples.size() * 2);
        frame.insert(frame.end(), meowlive::protocol::audio_magic.begin(),
                     meowlive::protocol::audio_magic.end());
        append_little_endian(frame, meowlive::protocol::version);
        append_little_endian(frame, static_cast<std::uint16_t>(chunk.end));
        append_little_endian(frame, chunk.generation);
        append_little_endian(frame, chunk.sequence);
        append_little_endian(frame, static_cast<std::uint16_t>(chunk.utterance_id.size()));
        append_little_endian(frame, static_cast<std::uint32_t>(chunk.samples.size()));
        frame.insert(frame.end(), chunk.utterance_id.begin(), chunk.utterance_id.end());
        for (const auto sample : chunk.samples)
        {
            append_little_endian(frame, std::bit_cast<std::uint16_t>(sample));
        }
        return frame;
    }

    std::expected<meowlive::protocol::AudioChunk, std::string>
    decode_audio_chunk(const std::span<const std::uint8_t> frame)
    {
        if (frame.size() < meowlive::protocol::audio_header_bytes ||
            frame.size() > meowlive::protocol::max_audio_frame_bytes ||
            !std::ranges::equal(frame.first(meowlive::protocol::audio_magic.size()),
                                meowlive::protocol::audio_magic))
        {
            return std::unexpected("invalid audio frame header or size");
        }
        const auto flags = read_little_endian<std::uint16_t>(frame, 6);
        if (read_little_endian<std::uint16_t>(frame, 4) != meowlive::protocol::version || flags > 1)
        {
            return std::unexpected("unsupported audio protocol version or flags");
        }
        const auto id_size = read_little_endian<std::uint16_t>(frame, 16);
        const auto sample_count = read_little_endian<std::uint32_t>(frame, 18);
        const auto payload_offset = meowlive::protocol::audio_header_bytes + id_size;
        if (id_size == 0 || id_size > meowlive::protocol::max_utterance_id_bytes ||
            sample_count > meowlive::protocol::max_audio_samples ||
            frame.size() != payload_offset + sample_count * 2)
        {
            return std::unexpected("invalid audio frame payload bounds");
        }
        const std::string_view id(
            reinterpret_cast<const char*>(frame.data() + meowlive::protocol::audio_header_bytes),
            id_size);
        if (!meowlive::protocol::is_utf8(id))
        {
            return std::unexpected("audio identity is not UTF-8");
        }

        meowlive::protocol::AudioChunk chunk{
            .utterance_id = std::string(id),
            .generation = read_little_endian<std::uint32_t>(frame, 8),
            .sequence = read_little_endian<std::uint32_t>(frame, 12),
            .samples = {},
            .end = flags == 1,
        };
        chunk.samples.reserve(sample_count);
        for (auto offset = payload_offset; offset < frame.size(); offset += 2)
        {
            chunk.samples.push_back(
                std::bit_cast<std::int16_t>(read_little_endian<std::uint16_t>(frame, offset)));
        }
        return chunk;
    }
}
