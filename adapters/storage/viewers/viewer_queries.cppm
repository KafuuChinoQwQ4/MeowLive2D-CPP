module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.viewer_queries;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.adapters.storage.relationship_admin;
import meowlive.adapters.storage.relationship_queries;
import meowlive.domain.live_event;
import meowlive.protocol.json;

namespace
{
    constexpr auto viewer_projection = R"sql(
SELECT jsonb_build_object(
  'viewer_id', page.id,
  'current_alias', page.current_alias,
  'alias_observed_at_ms', page.alias_observed_at_ms,
  'identities', COALESCE((
    SELECT jsonb_agg(to_jsonb(identity))
    FROM (SELECT platform, namespace, id_kind, external_id, last_confirmed_at_ms
          FROM viewer_identities WHERE scope_id = page.scope_id AND viewer_id = page.id
          ORDER BY last_confirmed_at_ms DESC, platform, namespace, id_kind, external_id
          LIMIT 64) identity
  ), '[]'::jsonb),
  'aliases', COALESCE((
    SELECT jsonb_agg(to_jsonb(alias_row))
    FROM (SELECT alias, first_seen_at_ms, last_seen_at_ms
          FROM viewer_aliases WHERE scope_id = page.scope_id AND viewer_id = page.id
          ORDER BY last_seen_at_ms DESC, alias LIMIT 64) alias_row
  ), '[]'::jsonb)
)
FROM page ORDER BY page.alias_observed_at_ms DESC NULLS LAST, page.id
)sql";

    ::Json::Value json_rows(const meowlive::adapters::storage::PostgresRows& rows)
    {
        ::Json::Value result(::Json::arrayValue);
        for (const auto& row : rows)
        {
            const auto parsed = meowlive::protocol::json::parse_object(row.at(0).value());
            if (!parsed)
            {
                throw std::runtime_error("viewer_store_reply_invalid");
            }
            result.append(*parsed);
        }
        return result;
    }
}

export namespace meowlive::adapters::storage
{
    class ViewerQueries
    {
    public:
        ViewerQueries(std::string url, std::filesystem::path libpq,
                      std::filesystem::path migrations, std::string scope)
            : url_(std::move(url)), libpq_(std::move(libpq)), migrations_(std::move(migrations)),
              scope_(std::move(scope))
        {
        }

        const std::string& scope() const
        {
            return scope_;
        }

        bool available()
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            return connection.execute("SELECT to_regclass('viewer_events') IS NOT NULL")
                       .at(0)
                       .at(0) == "t";
        }

        ::Json::Value viewers(unsigned limit, std::uint64_t offset)
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            const auto sql = std::string(R"sql(
WITH page AS (
  SELECT id, scope_id, current_alias, alias_observed_at_ms
  FROM viewers
  WHERE scope_id = $1 AND merged_into IS NULL
  ORDER BY alias_observed_at_ms DESC NULLS LAST, id
  LIMIT $2::int OFFSET $3::bigint
)
)sql") + viewer_projection;
            return json_rows(connection.execute(
                sql, std::array{scope_, std::to_string(limit), std::to_string(offset)}));
        }

        std::optional<::Json::Value> viewer(const std::string& id)
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            const auto sql = std::string(R"sql(
WITH page AS (
  SELECT id, scope_id, current_alias, alias_observed_at_ms
  FROM viewers WHERE scope_id = $1 AND id = $2::uuid AND merged_into IS NULL
)
)sql") + viewer_projection;
            auto rows = json_rows(connection.execute(sql, std::array{scope_, id}));
            return rows.empty() ? std::nullopt : std::optional(std::move(rows[0u]));
        }

        ::Json::Value relationships(const std::string& id, const ::Json::Value& references,
                                    std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            return meowlive::adapters::storage::list_viewer_relationships(connection, scope_, id,
                                                                          references, now);
        }

        std::optional<::Json::Value>
        edit_relationship(const std::string& id, const ::Json::Value& request, std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            return meowlive::adapters::storage::edit_relationship(connection, scope_, id, request,
                                                                  now);
        }

        ::Json::Value events(unsigned limit, std::uint64_t offset)
        {
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_);
            prepare(connection);
            return json_rows(connection.execute(
                R"sql(
SELECT jsonb_build_object(
  'event_id', event_id, 'source', source, 'session_id', session_id,
  'viewer_id', viewer_id, 'viewer', viewer_alias,
  'occurred_at_ms', occurred_at_ms, 'received_at_ms', received_at_ms,
  'kind', CASE event_type
    WHEN 'chat' THEN jsonb_build_object('type', 'chat', 'text', payload->'text')
    WHEN 'gift' THEN jsonb_build_object('type', 'gift', 'name', payload->'name', 'count', payload->'count')
    WHEN 'super_chat' THEN jsonb_build_object('type', 'super_chat', 'text', payload->'text',
      'amount_cny', payload->'amount_cny', 'start_at_ms', payload->'start_at_ms', 'end_at_ms', payload->'end_at_ms')
    ELSE jsonb_build_object('type', 'room_enter') END,
  'gift_metadata', payload->'gift_metadata'
)
FROM viewer_events
WHERE scope_id = $1
ORDER BY received_at_ms DESC, source, event_id
LIMIT $2::int OFFSET $3::bigint
)sql",
                std::array{scope_, std::to_string(limit), std::to_string(offset)}));
        }

        ::Json::Value memories(const meowlive::domain::LiveEvent& event, std::uint64_t now,
                               std::chrono::seconds timeout, const std::string& model = {},
                               unsigned dimensions = 0, const std::string& vector = {}) const
        {
            if (!event.viewer_identity)
            {
                return ::Json::Value(::Json::arrayValue);
            }
            const auto& identity = *event.viewer_identity;
            const auto kind =
                identity.kind == meowlive::domain::ViewerIdentityKind::Uid ? "uid" : "open_id";
            meowlive::adapters::storage::PostgresConnection connection(url_, libpq_, 2);
            connection.execute("SELECT set_config('statement_timeout', $1, false)",
                               std::array{std::to_string(timeout.count()) + "s"});
            return json_rows(connection.execute(
                R"sql(
SELECT jsonb_build_object('key', left(memory.key, 80), 'value', left(memory.value, 240),
                          'kind', memory.kind)
FROM viewer_events event
JOIN memories memory ON memory.scope_id = event.scope_id AND memory.viewer_id = event.viewer_id
LEFT JOIN memory_vectors v ON (v.scope_id,v.memory_id,v.version)=(memory.scope_id,memory.id,memory.version)
  AND v.model=$8 AND v.dimensions=$9::int
WHERE event.scope_id = $1 AND event.source = $2 AND event.event_id = $3
  AND NOT memory.deleted AND memory.status IN ('short_term', 'long_term')
  AND (memory.expires_at_ms IS NULL OR memory.expires_at_ms > $4::bigint)
  AND EXISTS (SELECT FROM viewer_identities identity
    WHERE identity.scope_id = event.scope_id AND identity.viewer_id = event.viewer_id
      AND identity.platform = event.source AND identity.namespace = $5
      AND identity.id_kind = $6 AND identity.external_id = $7)
ORDER BY (
  CASE WHEN memory.status='long_term' THEN 1.0
    ELSE 0.8*power(0.5,greatest(0,$4::bigint-coalesce((SELECT min(occurred_at_ms)
      FROM memory_evidence WHERE scope_id=memory.scope_id AND memory_id=memory.id),
      memory.created_at_ms))::double precision/604800000.0) END
  * CASE WHEN v.embedding IS NULL OR $10='' THEN 1.0
    ELSE 1.0/(1.0+(v.embedding <-> NULLIF($10,'')::vector)) END
) DESC, memory.updated_at_ms DESC, memory.id
LIMIT 5
)sql",
                std::array{scope_, event.source, event.id, std::to_string(now),
                           identity.identity_namespace, std::string(kind), identity.external_id,
                           model, std::to_string(dimensions), vector}));
        }

    private:
        void prepare(meowlive::adapters::storage::PostgresConnection& connection)
        {
            std::call_once(initialized_,
                           [&]
                           {
                               meowlive::adapters::storage::migrate_postgres(connection,
                                                                             migrations_);
                           });
            connection.execute("SET statement_timeout = '5s'");
        }

        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        std::once_flag initialized_;
    };
}
