module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.character_profiles;

import meowlive.standard;

import meowlive.application.speech_worker;
import meowlive.domain.character;
import meowlive.server.speech_voices;

export namespace meowlive::server
{
    enum class CharacterEdit
    {
        Save,
        Select,
        Delete,
        Preview,
    };

    using CharacterQuery = std::function<std::optional<::Json::Value>(const ::Json::Value&)>;
}

namespace
{
    using Edit = meowlive::server::CharacterEdit;
    using Error = meowlive::server::VoiceEditError;
    using Result = meowlive::server::VoiceLibrary::Result;

    Result invalid(std::string_view detail)
    {
        return std::unexpected(meowlive::server::VoiceEditFailure{Error::Invalid, detail});
    }

    bool fields(const ::Json::Value& object, std::initializer_list<std::string_view> names)
    {
        return object.isObject() &&
               std::ranges::all_of(object.getMemberNames(),
                                   [names](const auto& name)
                                   {
                                       return std::ranges::find(names, name) != names.end();
                                   });
    }

    bool valid_text(const ::Json::Value& value, std::size_t limit)
    {
        return value.isString() && meowlive::domain::valid_character_text(value.asString(), limit);
    }

    bool valid_mappings(::Json::Value& mappings)
    {
        if (!mappings.isArray() || mappings.size() > 32)
        {
            return false;
        }
        std::unordered_set<std::string> intents;
        for (auto& mapping : mappings)
        {
            if (!fields(mapping, {"intent", "hotkey_id", "fallback_hotkey_id", "validated"}) ||
                !valid_text(mapping["intent"], 40) || !valid_text(mapping["hotkey_id"], 128) ||
                (!mapping["fallback_hotkey_id"].isNull() &&
                 !valid_text(mapping["fallback_hotkey_id"], 128)) ||
                !intents.insert(mapping["intent"].asString()).second)
            {
                return false;
            }
            mapping["validated"] = false;
        }
        return true;
    }

    auto find_character(::Json::Value& characters, const ::Json::Value& id)
    {
        return std::find_if(characters.begin(), characters.end(),
                            [&id](const ::Json::Value& profile)
                            {
                                return profile["id"] == id;
                            });
    }

    Result save(::Json::Value& catalog, ::Json::Value profile, const std::string& new_id)
    {
        if (!fields(profile,
                    {"id", "name", "model_id", "voice_id", "mouth_parameter", "mappings"}) ||
            !valid_text(profile["name"], 80) || !valid_text(profile["model_id"], 128) ||
            !profile["voice_id"].isString() ||
            !(profile["voice_id"].asString().empty() ||
              meowlive::domain::valid_character_id(profile["voice_id"].asString())) ||
            !profile["mouth_parameter"].isString() ||
            !meowlive::domain::valid_mouth_parameter(profile["mouth_parameter"].asString()) ||
            !valid_mappings(profile["mappings"]))
        {
            return invalid("角色档案字段无效");
        }
        const auto id = profile["id"];
        auto& characters = catalog["characters"];
        if (id.isNull())
        {
            if (characters.size() >= 64)
            {
                return invalid("角色库已满");
            }
            profile["id"] = new_id;
            characters.append(std::move(profile));
            return {};
        }
        auto existing = find_character(characters, id);
        if (existing == characters.end() || !id.isString())
        {
            return invalid("角色不存在");
        }
        if (catalog["active_character_id"] == id &&
            (profile["model_id"] != (*existing)["model_id"] ||
             profile["mouth_parameter"] != (*existing)["mouth_parameter"] ||
             profile["voice_id"] != (*existing)["voice_id"]))
        {
            return invalid("请先选择另一角色，再编辑当前角色的模型、口型或音色");
        }
        *existing = std::move(profile);
        return {};
    }

    Result select(::Json::Value& catalog, const ::Json::Value& profile,
                  const meowlive::server::VoiceLibrary& library)
    {
        if (!profile["voice_id"].isString() ||
            !library.voice_available(profile["voice_id"].asString()) ||
            !valid_text(profile["model_id"], 128) || !profile["mouth_parameter"].isString() ||
            !meowlive::domain::valid_mouth_parameter(profile["mouth_parameter"].asString()))
        {
            return invalid("角色模型、口型或音色不可用");
        }
        catalog["active_character_id"] = profile["id"];
        catalog["active_voice_id"] = profile["voice_id"];
        return {};
    }

    Result preview(::Json::Value& profile, const ::Json::Value& intent,
                   const meowlive::server::CharacterQuery& query)
    {
        auto& mappings = profile["mappings"];
        const auto mapping = std::find_if(mappings.begin(), mappings.end(),
                                          [&intent](const ::Json::Value& item)
                                          {
                                              return item["intent"] == intent;
                                          });
        if (mapping == mappings.end())
        {
            return invalid("该角色未配置此动作");
        }
        ::Json::Value operation(::Json::objectValue);
        operation["type"] = "trigger_hotkey";
        operation["model_id"] = profile["model_id"];
        operation["hotkey_id"] = (*mapping)["hotkey_id"];
        operation["fallback_hotkey_id"] = (*mapping)["fallback_hotkey_id"];
        const auto result = query(operation);
        if (!result || (*result)["type"] != "hotkey_triggered" ||
            ((*result)["hotkey_id"] != operation["hotkey_id"] &&
             (*result)["hotkey_id"] != operation["fallback_hotkey_id"]))
        {
            return invalid("执行端未确认预览角色动作");
        }
        (*mapping)["validated"] = true;
        return {};
    }
    Result apply_character_edit(meowlive::server::CharacterEdit edit, ::Json::Value& catalog,
                                const ::Json::Value& input, const std::string& transaction,
                                const meowlive::server::VoiceLibrary& library,
                                const meowlive::server::CharacterQuery& query)
    {
        if (edit == Edit::Save)
        {
            return save(catalog, input, transaction);
        }
        const bool previewing = edit == Edit::Preview;
        if (!(previewing
                  ? fields(input, {"character_id", "intent"}) && valid_text(input["intent"], 40)
                  : fields(input, {"id"})))
        {
            return invalid("角色请求字段无效");
        }
        const auto& id = input[previewing ? "character_id" : "id"];
        if (!id.isString() || !meowlive::domain::valid_character_id(id.asString()))
        {
            return invalid("角色标识无效");
        }
        auto& characters = catalog["characters"];
        auto profile = find_character(characters, id);
        if (profile == characters.end())
        {
            return invalid("角色不存在");
        }
        if (edit == Edit::Select)
        {
            return select(catalog, *profile, library);
        }
        if (previewing)
        {
            return preview(*profile, input["intent"], query);
        }
        if (catalog["active_character_id"] == id)
        {
            catalog["active_character_id"] = ::Json::Value{};
        }
        ::Json::Value removed;
        characters.removeIndex(profile.index(), &removed);
        return {};
    }
}

export namespace meowlive::server
{
    meowlive::server::VoiceLibrary::Result
    edit_character(meowlive::server::CharacterEdit edit, meowlive::server::VoiceLibrary& library,
                   meowlive::application::SpeechWorker& speech, const ::Json::Value& input,
                   const std::string& transaction, const meowlive::server::CharacterQuery& query)
    {
        std::function<Result(const ::Json::Value&)> confirm;
        if (edit == Edit::Select)
        {
            confirm = [&](const ::Json::Value& catalog) -> Result
            {
                if (catalog["active_character_id"].isNull())
                {
                    return {};
                }
                const auto& characters = catalog["characters"];
                const auto profile =
                    std::ranges::find_if(characters,
                                         [&](const ::Json::Value& entry)
                                         {
                                             return entry["id"] == catalog["active_character_id"];
                                         });
                if (profile == characters.end())
                {
                    throw std::logic_error("selected_character_missing");
                }
                ::Json::Value operation(::Json::objectValue);
                operation["type"] = "load_model";
                operation["model_id"] = (*profile)["model_id"];
                operation["mouth_parameter"] = (*profile)["mouth_parameter"];
                const auto response = query(operation);
                if (!response || (*response)["type"] != "model_loaded" ||
                    (*response)["model_id"] != operation["model_id"])
                {
                    return invalid("执行端未确认加载角色模型");
                }
                return {};
            };
        }
        return library.edit_characters(
            speech,
            [&](::Json::Value& catalog)
            {
                return apply_character_edit(edit, catalog, input, transaction, library, query);
            },
            transaction, confirm);
    }
}
