module;

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.desktop_runtime.avatar.resources;

import meowlive.standard;
import meowlive.desktop_runtime.avatar.presentation;
import meowlive.protocol.resources;

namespace
{
    using Result = meowlive::protocol::DesktopResourceResult;
    using Reply = std::function<void(Result)>;

    void respond(::Json::Value value, const Reply& reply)
    {
        if (auto result = Result::decode(value))
        {
            reply(std::move(*result));
        }
    }

    void error(const ::QString& message, const Reply& reply)
    {
        ::Json::Value value(::Json::objectValue);
        value["type"] = "error";
        value["code"] = "avatar_error";
        value["message"] = message.toStdString();
        respond(std::move(value), reply);
    }

    ::Json::Value entries(const ::QJsonArray& items, const ::QString& id_field,
                          const ::QString& name_field)
    {
        ::Json::Value result(::Json::arrayValue);
        for (const auto& item : items)
        {
            const auto value = item.toObject();
            const auto id = value.value(id_field).toString();
            const auto name = value.value(name_field).toString();
            if (!id.isEmpty() && id.size() <= 128 && name.size() <= 256)
            {
                ::Json::Value entry(::Json::objectValue);
                entry["id"] = id.toStdString();
                entry["name"] = name.isEmpty() ? entry["id"].asString() : name.toStdString();
                result.append(std::move(entry));
            }
        }
        return result;
    }
}

export namespace meowlive::desktop_runtime::avatar
{
    bool query_resource(meowlive::desktop_runtime::avatar::Presentation& presentation,
                        const ::Json::Value& input,
                        std::function<void(meowlive::protocol::DesktopResourceResult)> reply)
    {
        auto failure = [reply](const ::QString& message)
        {
            error(message, reply);
        };
        const auto type = input["type"].asString();
        const auto model_id = ::QString::fromStdString(input["model_id"].asString());
        if (type == "list_models")
        {
            presentation.refresh_models(
                [reply](const ::QJsonObject& response)
                {
                    ::Json::Value result(::Json::objectValue);
                    result["type"] = "models";
                    result["models"] =
                        entries(response.value(QStringLiteral("availableModels")).toArray(),
                                QStringLiteral("modelID"), QStringLiteral("modelName"));
                    respond(std::move(result), reply);
                },
                failure);
            return true;
        }
        if (type == "list_hotkeys")
        {
            presentation.list_hotkeys(
                model_id,
                [reply, failure, model_id](const ::QJsonObject& response)
                {
                    if (response.value(QStringLiteral("modelID")) != model_id)
                    {
                        failure(QStringLiteral("VTube Studio 返回了不同的模型"));
                        return;
                    }
                    ::Json::Value result(::Json::objectValue);
                    result["type"] = "hotkeys";
                    result["model_id"] = model_id.toStdString();
                    result["hotkeys"] =
                        entries(response.value(QStringLiteral("availableHotkeys")).toArray(),
                                QStringLiteral("hotkeyID"), QStringLiteral("name"));
                    respond(std::move(result), reply);
                },
                failure);
            return true;
        }
        if (type == "load_model")
        {
            presentation.load_model(
                model_id, ::QString::fromStdString(input["mouth_parameter"].asString()),
                [reply, model_id](const ::QJsonObject&)
                {
                    ::Json::Value result(::Json::objectValue);
                    result["type"] = "model_loaded";
                    result["model_id"] = model_id.toStdString();
                    respond(std::move(result), reply);
                },
                failure);
            return true;
        }
        if (type == "trigger_hotkey")
        {
            const auto primary = ::QString::fromStdString(input["hotkey_id"].asString());
            const auto fallback = ::QString::fromStdString(input["fallback_hotkey_id"].asString());
            presentation.list_hotkeys(
                model_id,
                [&presentation, reply, failure, model_id, primary,
                 fallback](const ::QJsonObject& response)
                {
                    if (response.value(QStringLiteral("modelID")) != model_id)
                    {
                        failure(QStringLiteral("VTube Studio 返回了不同的模型"));
                        return;
                    }
                    bool has_primary = false;
                    bool has_fallback = false;
                    for (const auto& item :
                         response.value(QStringLiteral("availableHotkeys")).toArray())
                    {
                        const auto id =
                            item.toObject().value(QStringLiteral("hotkeyID")).toString();
                        has_primary |= id == primary;
                        has_fallback |= !fallback.isEmpty() && id == fallback;
                    }
                    if (!has_primary && !has_fallback)
                    {
                        failure(QStringLiteral("模型缺少指定快捷键"));
                        return;
                    }
                    const auto id = has_primary ? primary : fallback;
                    presentation.trigger_hotkey(
                        model_id, id,
                        [reply, id](const ::QJsonObject&)
                        {
                            ::Json::Value result(::Json::objectValue);
                            result["type"] = "hotkey_triggered";
                            result["hotkey_id"] = id.toStdString();
                            respond(std::move(result), reply);
                        },
                        failure);
                },
                failure);
            return true;
        }
        return false;
    }
}
