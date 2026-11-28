module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.repositories;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.application.live_session;
import meowlive.platform.exception_diagnostics;
import meowlive.domain.live_event;
import meowlive.protocol.json;

namespace
{
    using Connection = meowlive::adapters::storage::PostgresConnection;
    using Event = meowlive::domain::LiveEvent;

    const char* kind(const Event& value)
    {
        return std::visit(
            []<typename T>(const T&)
            {
                if constexpr (std::is_same_v<T, meowlive::domain::Chat>)
                {
                    return "chat";
                }
                else if constexpr (std::is_same_v<T, meowlive::domain::Gift>)
                {
                    return "gift";
                }
                else if constexpr (std::is_same_v<T, meowlive::domain::SuperChat>)
                {
                    return "super_chat";
                }
                else
                {
                    return "room_enter";
                }
            },
            value.kind);
    }

    const char* identity_kind(const meowlive::domain::ViewerIdentity& identity)
    {
        return identity.kind == meowlive::domain::ViewerIdentityKind::Uid ? "uid" : "open_id";
    }

    std::string payload(const Event& event)
    {
        ::Json::Value data(::Json::objectValue);
        std::visit(
            [&data]<typename T>(const T& value)
            {
                if constexpr (std::is_same_v<T, meowlive::domain::Chat> ||
                              std::is_same_v<T, meowlive::domain::SuperChat>)
                {
                    data["text"] = value.text;
                }
                if constexpr (std::is_same_v<T, meowlive::domain::Gift>)
                {
                    data["name"] = value.name;
                    data["count"] = value.count;
                }
                if constexpr (std::is_same_v<T, meowlive::domain::SuperChat>)
                {
                    data["amount_cny"] = value.amount_cny;
                    data["start_at_ms"] = static_cast<::Json::UInt64>(value.start_at_ms);
                    data["end_at_ms"] = static_cast<::Json::UInt64>(value.end_at_ms);
                }
            },
            event.kind);
        if (const auto& identity = event.viewer_identity)
        {
            auto& stored = data["viewer_identity"];
            stored["namespace"] = identity->identity_namespace;
            stored["id_kind"] = identity_kind(*identity);
            stored["external_id"] = identity->external_id;
        }
        if (const auto& gift = event.gift_metadata)
        {
            auto& stored = data["gift_metadata"] = ::Json::Value(::Json::objectValue);
            stored["price"] = gift->price ? ::Json::Value(static_cast<::Json::UInt64>(*gift->price))
                                          : ::Json::Value{};
            stored["paid"] = gift->paid ? ::Json::Value(*gift->paid) : ::Json::Value{};
            stored["medal_level"] =
                gift->medal_level ? ::Json::Value(*gift->medal_level) : ::Json::Value{};
            stored["guard_level"] =
                gift->guard_level ? ::Json::Value(*gift->guard_level) : ::Json::Value{};
        }
        return meowlive::protocol::json::serialize(data);
    }

    std::string resolve_viewer(Connection& db, const std::string& scope, const Event& event,
                               const std::string& received, const std::string& occurred)
    {
        const auto& identity = *event.viewer_identity;
        const std::array args{scope,
                              event.source,
                              identity.identity_namespace,
                              std::string(identity_kind(identity)),
                              identity.external_id,
                              received,
                              occurred};
        // The caller holds the same per-scope lock as Rust event ingestion and viewer merge.
        const auto viewer = db.execute(R"sql(
WITH existing AS (
    UPDATE viewer_identities
       SET last_confirmed_at_ms=GREATEST(last_confirmed_at_ms,$7::bigint)
     WHERE scope_id=$1 AND platform=$2 AND namespace=$3 AND id_kind=$4 AND external_id=$5
    RETURNING viewer_id
), created AS (
    INSERT INTO viewers(id,scope_id,created_at_ms)
    SELECT gen_random_uuid(),$1,$6::bigint WHERE NOT EXISTS (SELECT 1 FROM existing)
    RETURNING id
), linked AS (
    INSERT INTO viewer_identities(scope_id,platform,namespace,id_kind,external_id,viewer_id,last_confirmed_at_ms)
    SELECT $1,$2,$3,$4,$5,id,$7::bigint FROM created RETURNING viewer_id
)
SELECT viewer_id FROM existing UNION ALL SELECT viewer_id FROM linked
)sql",
                                       args);
        if (viewer.size() != 1 || !viewer.front().front())
        {
            throw std::runtime_error("viewer_identity_unresolved");
        }
        return *viewer.front().front();
    }

    bool ingest(Connection& db, const std::string& scope, const std::string& session,
                const Event& event, std::uint64_t received_at_ms,
                std::optional<int> memory_calendar_offset)
    {
        const auto occurred = std::to_string(event.occurred_at_ms);
        const auto received = std::to_string(received_at_ms);
        db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                   std::array{scope});
        db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                   std::array{scope});
        db.execute("INSERT INTO live_sessions(scope_id,session_id,first_received_at_ms)"
                   " VALUES($1,$2,$3::bigint) ON CONFLICT DO NOTHING",
                   std::array{scope, session, received});
        const auto inserted = db.execute(
            "INSERT INTO "
            "viewer_events(scope_id,source,event_id,session_id,viewer_alias,occurred_at_ms,"
            "received_at_ms,event_type,payload) "
            "VALUES($1,$2,$3,$4,$5,$6::bigint,$7::bigint,$8,$9::jsonb)"
            " ON CONFLICT DO NOTHING RETURNING event_id",
            std::array{scope, event.source, event.id, session, event.viewer, occurred, received,
                       std::string(kind(event)), payload(event)});
        if (inserted.empty())
        {
            return false;
        }
        if (event.viewer_identity)
        {
            const auto viewer = resolve_viewer(db, scope, event, received, occurred);
            db.execute("UPDATE viewer_events SET viewer_id=$4::uuid"
                       " WHERE scope_id=$1 AND source=$2 AND event_id=$3",
                       std::array{scope, event.source, event.id, viewer});
            db.execute(R"sql(
WITH alias AS (
    INSERT INTO viewer_aliases(scope_id,viewer_id,alias,first_seen_at_ms,last_seen_at_ms)
    VALUES($1,$2::uuid,$3,$4::bigint,$4::bigint)
    ON CONFLICT(scope_id,viewer_id,alias) DO UPDATE
      SET first_seen_at_ms=LEAST(viewer_aliases.first_seen_at_ms,excluded.first_seen_at_ms),
          last_seen_at_ms=GREATEST(viewer_aliases.last_seen_at_ms,excluded.last_seen_at_ms)
)
UPDATE viewers SET current_alias=$3, alias_observed_at_ms=$4::bigint
 WHERE scope_id=$1 AND id=$2::uuid AND (alias_observed_at_ms IS NULL OR alias_observed_at_ms <= $4::bigint)
)sql",
                       std::array{scope, viewer, event.viewer, occurred});
            if (memory_calendar_offset &&
                std::holds_alternative<meowlive::domain::Chat>(event.kind))
            {
                db.execute(R"sql(
INSERT INTO memory_jobs(id,scope_id,viewer_id,source,event_id,body,occurred_at_ms,day,available_at_ms)
SELECT gen_random_uuid(),scope_id,viewer_id,source,event_id,payload->>'text',occurred_at_ms,
    floor((occurred_at_ms::numeric+$5::bigint*60000)/86400000)::bigint,$4::bigint
FROM viewer_events WHERE scope_id=$1 AND source=$2 AND event_id=$3
    AND length(payload->>'text')>0
ON CONFLICT(scope_id,source,event_id) DO NOTHING
)sql",
                           std::array{scope, event.source, event.id, received,
                                      std::to_string(*memory_calendar_offset)});
            }
        }
        return true;
    }
}

export namespace meowlive::adapters::storage
{
    // The application event queue owns calls to this repository.
    class ViewerEvents
    {
    public:
        ViewerEvents(std::string database_url, std::filesystem::path libpq,
                     std::filesystem::path migrations, std::string scope, std::string run_id,
                     std::optional<int> memory_calendar_offset = {})
            : url_(std::move(database_url)), libpq_(std::move(libpq)),
              migrations_(std::move(migrations)), scope_(std::move(scope)),
              run_id_(std::move(run_id)), memory_calendar_offset_(memory_calendar_offset)
        {
        }

        meowlive::application::LivePersistence append(std::uint64_t session, const Event& event,
                                                      std::uint64_t received_at_ms)
        {
            using Result = meowlive::application::LivePersistence;
            try
            {
                if (!db_)
                {
                    db_.emplace(url_, libpq_);
                    meowlive::adapters::storage::migrate_postgres(*db_, migrations_);
                }
                db_->execute("BEGIN");
                db_->execute("SET LOCAL statement_timeout='5s'");
                const auto added =
                    ingest(*db_, scope_, "live:" + run_id_ + ":" + std::to_string(session), event,
                           received_at_ms, memory_calendar_offset_);
                db_->execute("COMMIT");
                return added ? Result::Stored : Result::Duplicate;
            }
            catch (const meowlive::adapters::storage::PostgresUnavailable&)
            {
                db_.reset();
                meowlive::platform::log_current_exception("viewer.event.append");
                return Result::Failed;
            }
            catch (const meowlive::adapters::storage::PostgresError&)
            {
                db_.reset();
                meowlive::platform::log_current_exception("viewer.event.append");
                throw;
            }
            catch (...)
            {
                // Closing the connection also rolls back its unfinished transaction.
                db_.reset();
                throw;
            }
        }

    private:
        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        const std::string run_id_;
        const std::optional<int> memory_calendar_offset_;
        std::optional<meowlive::adapters::storage::PostgresConnection> db_;
    };
}
