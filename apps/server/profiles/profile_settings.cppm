module;

#include <drogon/utils/Utilities.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.profile_settings;

import meowlive.standard;

export import meowlive.server.profile_defaults;
export import meowlive.server.profile_edits;
import meowlive.application.work_queue;
import meowlive.protocol.json;
import meowlive.server.profile_file;
import meowlive.server.settings;

namespace
{
    ::Json::Value empty_profiles()
    {
        ::Json::Value document(::Json::objectValue);
        document["schema"] = 2;
        document["profiles"] = ::Json::Value(::Json::arrayValue);
        document["selected_profile_id"] = ::Json::Value{};
        return document;
    }

    std::filesystem::path profile_path(const std::filesystem::path& config, std::string_view suffix)
    {
        auto filename = config.stem();
        filename += suffix;
        return config.parent_path() / "local" / filename;
    }

    bool persist(const std::filesystem::path& path, const ::Json::Value& document,
                 std::size_t limit)
    {
        const auto bytes = meowlive::protocol::json::serialize(document);
        if (bytes.size() > limit)
        {
            return false;
        }
        const auto temporary =
            path.parent_path() / (".profile-" + drogon::utils::getUuid() + ".tmp");
        return meowlive::server::write_profile_file(path, temporary, bytes);
    }
}

export namespace meowlive::server
{
    class ProfileSettings
    {
    public:
        using Result = std::expected<::Json::Value, meowlive::server::ProfileError>;
        using Apply = std::function<void(const ::Json::Value&, const ::Json::Value&)>;

        ProfileSettings(const std::filesystem::path& config,
                        meowlive::server::ServerSettings settings, ::Json::Value agent_defaults,
                        ::Json::Value llm_defaults)
            : agent_path_(profile_path(config, "-agent.json")),
              llm_path_(profile_path(config, "-llm.json")),
              agent_defaults_(std::move(agent_defaults)), llm_defaults_(std::move(llm_defaults)),
              agent_(settings.take_agent().value_or(empty_profiles())),
              llm_(settings.take_llm().value_or(empty_profiles())),
              active_model_(meowlive::server::selected_model(llm_, llm_defaults_, llm_defaults_))
        {
            agent_["settings"] =
                meowlive::server::with_profile_defaults(agent_defaults_, agent_["settings"]);
            active_model_["api_key"];
            if (!meowlive::server::valid_agent_profile_settings(agent_["settings"]))
            {
                throw std::runtime_error("本机设置无效 [agent.settings]");
            }
            if (!meowlive::server::valid_model_profile_settings(active_model_))
            {
                throw std::runtime_error("本机设置无效 [llm.settings]");
            }
        }

        void set_apply(Apply apply)
        {
            const std::scoped_lock lock(mutex_);
            apply_ = std::move(apply);
        }

        ::Json::Value agent_settings() const
        {
            const std::scoped_lock lock(mutex_);
            return agent_["settings"];
        }

        ::Json::Value model_config() const
        {
            return active_model_;
        }

        Result model_candidate(const ::Json::Value& request, bool listing = false) const
        {
            const std::scoped_lock lock(mutex_);
            return meowlive::server::draft_model(
                request, meowlive::server::selected_model(llm_, llm_defaults_, active_model_),
                listing);
        }

        ::Json::Value snapshot(bool model) const
        {
            const std::scoped_lock lock(mutex_);
            if (model)
            {
                return meowlive::server::model_profile_snapshot(llm_, llm_defaults_, active_model_);
            }
            ::Json::Value answer(::Json::objectValue);
            answer["profiles"] = agent_["profiles"];
            answer["selected_profile_id"] = agent_["selected_profile_id"];
            answer["storage_available"] = true;
            return answer;
        }

        meowlive::application::WorkSubmission submit(meowlive::application::WorkQueue::Task task)
        {
            return queue_.try_submit(std::move(task));
        }

        Result edit(bool model, meowlive::server::ProfileEdit edit, const ::Json::Value& request)
        {
            return model ? edit_llm(edit, request) : edit_agent(edit, request);
        }

        void close()
        {
            queue_.close();
        }

        void drain()
        {
            queue_.drain();
        }

    private:
        Result edit_agent(meowlive::server::ProfileEdit edit, const ::Json::Value& request)
        {
            ::Json::Value next;
            Apply apply;
            {
                const std::scoped_lock lock(mutex_);
                next = agent_;
                apply = apply_;
            }
            const auto changed =
                meowlive::server::edit_persona(next, edit, request, agent_defaults_);
            if (!changed)
            {
                return std::unexpected(changed.error());
            }
            ::Json::Value settings_to_apply;
            if (*changed && apply)
            {
                settings_to_apply = next["settings"];
            }
            if (!persist(agent_path_, next, 512 * 1024))
            {
                return std::unexpected(meowlive::server::ProfileError::Storage);
            }
            {
                const std::scoped_lock lock(mutex_);
                agent_ = std::move(next);
            }
            if (*changed && apply)
            {
                apply(settings_to_apply, active_model_);
            }
            return snapshot(false);
        }

        Result edit_llm(meowlive::server::ProfileEdit edit, const ::Json::Value& request)
        {
            ::Json::Value next;
            {
                const std::scoped_lock lock(mutex_);
                next = llm_;
            }
            const auto changed =
                meowlive::server::edit_model(next, edit, request, llm_defaults_, active_model_);
            if (!changed)
            {
                return std::unexpected(changed.error());
            }
            if (!persist(llm_path_, next, 128 * 1024))
            {
                return std::unexpected(meowlive::server::ProfileError::Storage);
            }
            {
                const std::scoped_lock lock(mutex_);
                llm_ = std::move(next);
            }
            return snapshot(true);
        }

        const std::filesystem::path agent_path_;
        const std::filesystem::path llm_path_;
        const ::Json::Value agent_defaults_;
        const ::Json::Value llm_defaults_;
        mutable std::mutex mutex_;
        ::Json::Value agent_;
        ::Json::Value llm_;
        ::Json::Value active_model_;
        Apply apply_;
        meowlive::application::WorkQueue queue_{32};
    };
}
