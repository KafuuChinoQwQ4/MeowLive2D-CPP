module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.protocol.resources;

import meowlive.standard;
import meowlive.protocol.json;

namespace
{
    enum class FieldType
    {
        String,
        OptionalString,
        Unsigned,
        Boolean,
        Object,
        Array,
    };

    struct Field
    {
        const char* name;
        FieldType type;
    };

    bool matches_type(const Json::Value& value, const FieldType type)
    {
        switch (type)
        {
        case FieldType::String:
            return value.isString();
        case FieldType::OptionalString:
            return value.isNull() || value.isString();
        case FieldType::Unsigned:
            return meowlive::protocol::json::is_unsigned_32(value);
        case FieldType::Boolean:
            return value.isBool();
        case FieldType::Object:
            return value.isObject();
        case FieldType::Array:
            return value.isArray();
        }
        return false;
    }

    bool matches_fields(const Json::Value& value, const std::initializer_list<Field> fields,
                        const bool strict = false)
    {
        if (!value.isObject())
        {
            return false;
        }
        const auto valid_fields =
            std::ranges::all_of(fields,
                                [&value](const Field& field)
                                {
                                    return matches_type(value[field.name], field.type);
                                });
        if (!valid_fields || !strict)
        {
            return valid_fields;
        }
        return std::ranges::all_of(value.getMemberNames(),
                                   [fields](const std::string& name)
                                   {
                                       return std::ranges::any_of(fields,
                                                                  [&name](const Field& field)
                                                                  {
                                                                      return name == field.name;
                                                                  });
                                   });
    }

    bool valid_obs_operation(const Json::Value& value)
    {
        const auto tag = value["type"].asString();
        if (tag == "status" || tag == "start_recording" || tag == "stop_recording")
        {
            return value.size() == 1;
        }
        return tag == "set_scene" &&
               matches_fields(
                   value, {{"type", FieldType::String}, {"scene_name", FieldType::String}}, true);
    }

    bool valid_resource_operation(Json::Value& value)
    {
        if (!value["type"].isString())
        {
            return false;
        }
        const auto tag = value["type"].asString();
        if (tag == "obs_settings" || tag == "list_models" || tag == "import_model" ||
            tag == "list_imported_models")
        {
            // Serde ignores extra fields on internally tagged unit variants.
            value = Json::Value(Json::objectValue);
            value["type"] = tag;
            return true;
        }
        if (tag == "save_obs_settings")
        {
            return matches_fields(value,
                                  {{"type", FieldType::String}, {"settings", FieldType::Object}},
                                  true) &&
                   matches_fields(value["settings"],
                                  {{"enabled", FieldType::Boolean},
                                   {"websocket_url", FieldType::String},
                                   {"password", FieldType::OptionalString},
                                   {"clear_password", FieldType::Boolean}},
                                  true);
        }
        if (tag == "obs")
        {
            return matches_fields(value,
                                  {{"type", FieldType::String}, {"operation", FieldType::Object}},
                                  true) &&
                   value["operation"]["type"].isString() && valid_obs_operation(value["operation"]);
        }
        if (tag == "load_model")
        {
            return matches_fields(value,
                                  {{"type", FieldType::String},
                                   {"model_id", FieldType::String},
                                   {"mouth_parameter", FieldType::String}},
                                  true);
        }
        if (tag == "list_hotkeys")
        {
            return matches_fields(
                value, {{"type", FieldType::String}, {"model_id", FieldType::String}}, true);
        }
        if (tag == "trigger_hotkey")
        {
            return matches_fields(value,
                                  {{"type", FieldType::String},
                                   {"model_id", FieldType::String},
                                   {"hotkey_id", FieldType::String},
                                   {"fallback_hotkey_id", FieldType::OptionalString}},
                                  true);
        }
        return tag == "delete_imported_model" &&
               matches_fields(value, {{"type", FieldType::String}, {"id", FieldType::String}},
                              true);
    }

    bool named_id_list(const Json::Value& value, const bool imported = false)
    {
        return value.isArray() &&
               std::ranges::all_of(
                   value,
                   [imported](const Json::Value& item)
                   {
                       return matches_fields(
                                  item, {{"id", FieldType::String}, {"name", FieldType::String}}) &&
                              (!imported ||
                               matches_type(item["model_id"], FieldType::OptionalString));
                   });
    }

    bool valid_resource_result(const Json::Value& value)
    {
        if (!value["type"].isString())
        {
            return false;
        }
        const auto tag = value["type"].asString();
        if (tag == "obs_settings")
        {
            return matches_fields(value["settings"],
                                  {{"enabled", FieldType::Boolean},
                                   {"websocket_url", FieldType::String},
                                   {"password_configured", FieldType::Boolean},
                                   {"storage_available", FieldType::Boolean}},
                                  true);
        }
        if (tag == "obs")
        {
            const auto& snapshot = value["snapshot"];
            return matches_fields(snapshot,
                                  {{"connected", FieldType::Boolean},
                                   {"recording", FieldType::Boolean},
                                   {"current_scene", FieldType::String},
                                   {"scenes", FieldType::Array}},
                                  true) &&
                   std::ranges::all_of(snapshot["scenes"],
                                       [](const Json::Value& scene)
                                       {
                                           return scene.isString();
                                       });
        }
        if (tag == "models" || tag == "imported_models")
        {
            return named_id_list(value["models"], tag == "imported_models");
        }
        if (tag == "model_loaded")
        {
            return value["model_id"].isString();
        }
        if (tag == "hotkeys")
        {
            return value["model_id"].isString() && named_id_list(value["hotkeys"]);
        }
        if (tag == "hotkey_triggered")
        {
            return value["hotkey_id"].isString();
        }
        if (tag == "model_imported")
        {
            return matches_fields(value, {{"model_name", FieldType::String},
                                          {"model_file", FieldType::String},
                                          {"files", FieldType::Unsigned},
                                          {"bytes", FieldType::Unsigned},
                                          {"restart_required", FieldType::Boolean}});
        }
        if (tag == "model_deleted")
        {
            return matches_fields(
                value, {{"id", FieldType::String}, {"restart_required", FieldType::Boolean}});
        }
        return tag == "error" &&
               matches_fields(value, {{"code", FieldType::String}, {"message", FieldType::String}});
    }
}

export namespace meowlive::protocol
{
    class DesktopResourceOperation
    {
    public:
        static std::expected<meowlive::protocol::DesktopResourceOperation, std::string>
        decode(const Json::Value& payload)
        {
            auto value = payload;
            if (!value.isObject() || !valid_resource_operation(value))
            {
                return std::unexpected("invalid desktop resource operation");
            }
            return meowlive::protocol::DesktopResourceOperation{std::move(value)};
        }

        const Json::Value& json() const
        {
            return payload_;
        }

    private:
        explicit DesktopResourceOperation(Json::Value payload) : payload_(std::move(payload))
        {
        }

        Json::Value payload_;
    };

    class DesktopResourceResult
    {
    public:
        static std::expected<meowlive::protocol::DesktopResourceResult, std::string>
        decode(const Json::Value& payload)
        {
            if (!payload.isObject() || !valid_resource_result(payload))
            {
                return std::unexpected("invalid desktop resource result");
            }
            return meowlive::protocol::DesktopResourceResult{payload};
        }

        const Json::Value& json() const
        {
            return payload_;
        }

    private:
        explicit DesktopResourceResult(const Json::Value& payload) : payload_(payload)
        {
        }

        Json::Value payload_;
    };
}
