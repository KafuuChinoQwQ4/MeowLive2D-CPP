module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.json;

import meowlive.standard;
import meowlive.protocol.utf8;

namespace
{
    constexpr ::Json::UInt json_depth_limit = 128;

    bool is_digit(char character)
    {
        return character >= '0' && character <= '9';
    }

    bool read_digits(std::string_view& text)
    {
        const auto count = text.find_first_not_of("0123456789");
        if (count == 0 || text.empty())
        {
            return false;
        }
        text.remove_prefix(count == std::string_view::npos ? text.size() : count);
        return true;
    }

    bool read_number(std::string_view& text)
    {
        if (text.starts_with('-'))
        {
            text.remove_prefix(1);
        }
        if (text.starts_with('0'))
        {
            text.remove_prefix(1);
            if (!text.empty() && is_digit(text.front()))
            {
                return false;
            }
        }
        else if (!read_digits(text))
        {
            return false;
        }
        if (text.starts_with('.'))
        {
            text.remove_prefix(1);
            if (!read_digits(text))
            {
                return false;
            }
        }
        if (text.starts_with('e') || text.starts_with('E'))
        {
            text.remove_prefix(1);
            if (text.starts_with('+') || text.starts_with('-'))
            {
                text.remove_prefix(1);
            }
            return read_digits(text);
        }
        return true;
    }

    bool read_code_unit(std::string_view& text, unsigned& value)
    {
        if (text.size() < 6 || !text.starts_with("\\u"))
        {
            return false;
        }
        const auto end = text.data() + 6;
        const auto result = std::from_chars(text.data() + 2, end, value, 16);
        if (result.ec != std::errc{} || result.ptr != end)
        {
            return false;
        }
        text.remove_prefix(6);
        return true;
    }

    bool read_escape(std::string_view& text)
    {
        if (!text.starts_with("\\u"))
        {
            if (text.size() < 2 || !std::string_view{"\"\\/bfnrt"}.contains(text[1]))
            {
                return false;
            }
            text.remove_prefix(2);
            return true;
        }
        unsigned code_unit{};
        if (!read_code_unit(text, code_unit) || (code_unit >= 0xdc00 && code_unit <= 0xdfff))
        {
            return false;
        }
        if (code_unit >= 0xd800 && code_unit <= 0xdbff)
        {
            return read_code_unit(text, code_unit) && code_unit >= 0xdc00 && code_unit <= 0xdfff;
        }
        return true;
    }

    bool valid_json_text(const std::string_view text)
    {
        if (!meowlive::protocol::is_utf8(text))
        {
            return false;
        }
        // JsonCpp's strict mode accepts some invalid number and Unicode escapes.
        bool quoted{};
        std::array<char, json_depth_limit> closing;
        std::size_t depth{};
        auto remaining = text;
        while (!remaining.empty())
        {
            const auto character = remaining.front();
            const auto whitespace =
                character == ' ' || character == '\t' || character == '\r' || character == '\n';
            if (static_cast<unsigned char>(character) < 0x20 && (quoted || !whitespace))
            {
                return false;
            }
            if (quoted && character == '\\')
            {
                if (!read_escape(remaining))
                {
                    return false;
                }
                continue;
            }
            if (!quoted && !whitespace)
            {
                if (character == ']' || character == '}')
                {
                    if (depth == 0 || character != closing[depth - 1])
                    {
                        return false;
                    }
                    --depth;
                }
                else if (depth == json_depth_limit)
                {
                    // Only an empty container fits at the last reader stack level.
                    return false;
                }
                else if (character == '[' || character == '{')
                {
                    closing[depth++] = character == '[' ? ']' : '}';
                }
                else if (is_digit(character) || character == '-')
                {
                    if (!read_number(remaining))
                    {
                        return false;
                    }
                    continue;
                }
            }
            if (character == '"')
            {
                quoted = !quoted;
            }
            remaining.remove_prefix(1);
        }
        return !quoted && depth == 0;
    }

    bool valid_json_strings(const Json::Value& value)
    {
        if (value.isString())
        {
            return meowlive::protocol::is_utf8(value.asString());
        }
        if (value.isArray() || value.isObject())
        {
            for (auto item = value.begin(); item != value.end(); ++item)
            {
                if ((value.isObject() && !meowlive::protocol::is_utf8(item.name())) ||
                    !valid_json_strings(*item))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

export namespace meowlive::protocol::json
{
    std::optional<Json::Value> parse_container(const std::string_view text)
    {
        if (text.empty() || !valid_json_text(text))
        {
            return std::nullopt;
        }
        Json::CharReaderBuilder builder;
        Json::CharReaderBuilder::strictMode(&builder.settings_);
        builder["collectComments"] = false;
        builder["skipBom"] = false;
        builder["stackLimit"] = json_depth_limit;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value value;
        if (reader->parse(text.data(), text.data() + text.size(), &value, nullptr) &&
            (value.isObject() || value.isArray()) && valid_json_strings(value))
        {
            return value;
        }
        return std::nullopt;
    }

    std::optional<Json::Value> parse_object(const std::string_view text)
    {
        auto value = meowlive::protocol::json::parse_container(text);
        if (value && value->isObject())
        {
            return value;
        }
        return std::nullopt;
    }

    std::string serialize(const Json::Value& value)
    {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        writer["emitUTF8"] = true;
        return Json::writeString(writer, value);
    }

    bool is_unsigned_32(const Json::Value& value)
    {
        return (value.type() == Json::intValue || value.type() == Json::uintValue) &&
               value.isUInt();
    }
}
