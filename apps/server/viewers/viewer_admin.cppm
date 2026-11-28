module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.viewer_admin;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.viewer_queries;
import meowlive.adapters.storage.viewer_affinity;
import meowlive.adapters.storage.viewer_merge;
import meowlive.adapters.llm.memory_embedding;
import meowlive.application.memory_lookup;
import meowlive.domain.live_event;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;

export namespace meowlive::server
{
    class ViewerAdmin
    {
    public:
        ViewerAdmin(std::string scope,
                    std::shared_ptr<meowlive::adapters::storage::ViewerQueries> queries,
                    std::shared_ptr<meowlive::adapters::storage::ViewerAffinity> affinity = {},
                    std::shared_ptr<meowlive::adapters::storage::ViewerMerge> merge = {})
            : scope_(std::move(scope)), queries_(std::move(queries)),
              affinity_(std::move(affinity)), merge_(std::move(merge))
        {
        }

        void report_unconfirmed() noexcept
        {
            ++unconfirmed_;
        }

        ::Json::Value status()
        {
            ::Json::Value result(::Json::objectValue);
            result["scope_id"] = scope_;
            result["enabled"] = static_cast<bool>(queries_);
            bool available{};
            if (queries_)
            {
                try
                {
                    available = queries_->available();
                }
                catch (const meowlive::adapters::storage::PostgresUnavailable&)
                {
                    meowlive::platform::log_current_exception("viewer.status");
                }
            }
            result["available"] = available;
            return result;
        }

        bool enabled() const noexcept
        {
            return static_cast<bool>(queries_);
        }

        ::Json::Value list(unsigned limit, std::uint64_t offset)
        {
            auto result = page(offset);
            result["viewers"] = queries_->viewers(limit, offset);
            return result;
        }

        std::optional<::Json::Value> detail(const std::string& id)
        {
            return queries_->viewer(id);
        }

        ::Json::Value relationships(const std::string& id, const ::Json::Value& references)
        {
            return queries_->relationships(id, references, current_utc_ms());
        }

        std::optional<::Json::Value> edit_relationship(const std::string& id,
                                                       const ::Json::Value& request)
        {
            return queries_->edit_relationship(id, request, current_utc_ms());
        }

        std::optional<::Json::Value> companionship(const std::string& viewer)
        {
            return affinity_->details(viewer);
        }

        std::optional<::Json::Value> preview_merge(const std::string& source,
                                                   const std::string& target)
        {
            return merge_->preview(source, target);
        }

        std::optional<::Json::Value>
        apply_merge(const meowlive::adapters::storage::ViewerMergeRequest& request)
        {
            return merge_->apply(request, current_utc_ms());
        }

        std::optional<std::string> change_affinity(const std::string& viewer,
                                                   const std::string& request_key,
                                                   const std::string& reason, int delta,
                                                   const std::string& reversed)
        {
            return affinity_->change(viewer, request_key, reason, delta, reversed,
                                     current_utc_ms());
        }

        ::Json::Value events(unsigned limit, std::uint64_t offset)
        {
            auto result = page(offset);
            result["unconfirmed_events"] = static_cast<::Json::UInt64>(unconfirmed_.load());
            result["events"] = queries_->events(limit, offset);
            return result;
        }

        std::expected<std::string, std::string>
        memory(const meowlive::domain::LiveEvent& event, std::chrono::seconds timeout,
               std::stop_token stop, const meowlive::adapters::EmbeddingConfig& embedding)
        {
            if (!queries_ || stop.stop_requested())
            {
                return std::unexpected("viewer_memory_unavailable");
            }
            try
            {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                ::Json::Value result(::Json::objectValue);
                std::string vector;
                if (const auto* chat = std::get_if<meowlive::domain::Chat>(&event.kind);
                    chat && !embedding.model.empty() && event.viewer_identity &&
                    timeout.count() > 1)
                {
                    if (auto encoded = meowlive::adapters::embed_memory(
                            embedding, chat->text, (std::min)(timeout / 2, std::chrono::seconds(3)),
                            stop))
                    {
                        vector = std::move(*encoded);
                    }
                }
                if (stop.stop_requested())
                {
                    return std::unexpected("viewer_memory_unavailable");
                }
                result["memories"] =
                    queries_->memories(event, static_cast<std::uint64_t>(now), timeout,
                                       embedding.model, embedding.dimensions, vector);
                if (!stop.stop_requested())
                {
                    return meowlive::protocol::json::serialize(result);
                }
            }
            catch (const meowlive::adapters::storage::PostgresUnavailable&)
            {
                meowlive::platform::log_current_exception("viewer.memory");
            }
            catch (const meowlive::adapters::storage::PostgresError&)
            {
                meowlive::platform::log_current_exception("viewer.memory");
                throw;
            }
            return std::unexpected("viewer_memory_unavailable");
        }

    private:
        static std::uint64_t current_utc_ms()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        ::Json::Value page(std::uint64_t offset) const
        {
            ::Json::Value result(::Json::objectValue);
            result["scope_id"] = scope_;
            result["offset"] = static_cast<::Json::UInt64>(offset);
            return result;
        }

        const std::string scope_;
        const std::shared_ptr<meowlive::adapters::storage::ViewerQueries> queries_;
        const std::shared_ptr<meowlive::adapters::storage::ViewerAffinity> affinity_;
        const std::shared_ptr<meowlive::adapters::storage::ViewerMerge> merge_;
        std::atomic<std::uint64_t> unconfirmed_{};
    };
}
