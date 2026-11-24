module;

#include <json/json.h>

#if defined(_WIN32)
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#endif

export module meowlive.protocol.json;

#if !defined(_WIN32)
import std;
#endif
import meowlive.protocol.utf8;

namespace
{
    bool valid_json_text(const std::string_view text)
    {
        bool quoted{};
        bool escaped{};
        for (const auto character : text)
        {
            if (static_cast<unsigned char>(character) < 0x20 &&
                (quoted || (character != '\t' && character != '\r' && character != '\n')))
            {
                return false;
            }
            if (escaped)
            {
                escaped = false;
            }
            else if (quoted && character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                quoted = !quoted;
            }
        }
        return meowlive::protocol::is_utf8(text);
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
    std::optional<Json::Value> parse_object(const std::string_view text)
    {
        if (text.empty() || !valid_json_text(text))
        {
            return std::nullopt;
        }
        Json::CharReaderBuilder builder;
        Json::CharReaderBuilder::strictMode(&builder.settings_);
        builder["collectComments"] = false;
        builder["skipBom"] = false;
        builder["stackLimit"] = 128;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value value;
        try
        {
            if (reader->parse(text.data(), text.data() + text.size(), &value, nullptr) &&
                value.isObject() && valid_json_strings(value))
            {
                return value;
            }
        }
        catch (const Json::RuntimeError&)
        {
            // JsonCpp reports its nesting limit through an exception.
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
