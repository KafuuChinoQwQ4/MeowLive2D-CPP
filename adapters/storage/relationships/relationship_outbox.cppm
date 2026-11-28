module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.relationship_outbox;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.protocol.json;

export namespace meowlive::adapters::storage
{
    struct ProjectionLease
    {
        std::string id;
        std::string token;
        ::Json::Value fact;
    };

    class RelationshipOutbox
    {
    public:
        RelationshipOutbox(std::string url, std::filesystem::path libpq,
                           std::filesystem::path migrations, std::string scope)
            : url_(std::move(url)), libpq_(std::move(libpq)), migrations_(std::move(migrations)),
              scope_(std::move(scope))
        {
        }

        std::optional<meowlive::adapters::storage::ProjectionLease> claim(std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            const std::array args{scope_, std::to_string(now)};
            db.execute("BEGIN");
            db.execute(R"sql(
UPDATE relationship_outbox SET state='failed',token=NULL,lease_until_ms=NULL
WHERE scope_id=$1 AND state='leased' AND lease_until_ms<=$2::bigint AND attempts>=3
)sql",
                       args);
            const auto rows = db.execute(R"sql(
WITH next AS (
  SELECT fact_id FROM relationship_outbox WHERE scope_id=$1 AND attempts<3 AND
    ((state='pending' AND available_at_ms<=$2::bigint) OR
     (state='leased' AND lease_until_ms<=$2::bigint))
  ORDER BY created_at_ms,fact_id LIMIT 1 FOR UPDATE SKIP LOCKED
), leased AS (
  UPDATE relationship_outbox q SET state='leased',attempts=attempts+1,
    token=gen_random_uuid(),lease_until_ms=$2::bigint+30000
  FROM next WHERE q.scope_id=$1 AND q.fact_id=next.fact_id
  RETURNING q.fact_id,q.token
)
SELECT l.fact_id,l.token,jsonb_build_object(
 'scope',f.scope_id,'id',f.id,'version',f.version,'deleted',f.deleted,
 'confirmed',f.confirmation='confirmed','expires',f.expires_at_ms,
 'source_kind',f.source_kind,'source_id',f.source_id,
 'target_kind',f.target_kind,'target_id',f.target_id,'kind',f.kind)
FROM leased l JOIN relationship_facts f ON f.scope_id=$1 AND f.id=l.fact_id
)sql",
                                         args);
            if (rows.empty())
            {
                db.execute("COMMIT");
                return {};
            }
            const auto& row = rows.at(0);
            auto fact = meowlive::protocol::json::parse_object(row.at(2).value());
            if (!fact)
            {
                throw std::runtime_error("graph_projection_record_invalid");
            }
            db.execute("COMMIT");
            return meowlive::adapters::storage::ProjectionLease{
                row.at(0).value(), row.at(1).value(), std::move(*fact)};
        }

        void finish(const meowlive::adapters::storage::ProjectionLease& lease, std::uint64_t now,
                    bool projected)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            db.execute(R"sql(
UPDATE relationship_outbox SET
 state=CASE WHEN $5::bool THEN 'done' WHEN attempts>=3 THEN 'failed' ELSE 'pending' END,
 available_at_ms=$4::bigint+attempts*1000, token=NULL, lease_until_ms=NULL
WHERE scope_id=$1 AND fact_id=$2::uuid AND token=$3::uuid
  AND state='leased' AND lease_until_ms>$4::bigint
)sql",
                       std::array{scope_, lease.id, lease.token, std::to_string(now),
                                  std::string(projected ? "true" : "false")});
        }

        ::Json::Value status(std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            const auto rows = db.execute(R"sql(
SELECT jsonb_build_object('pending',count(*) FILTER (WHERE state='pending'),
       'leased',count(*) FILTER (WHERE state='leased'),'failed',count(*) FILTER (WHERE state='failed'),
       'oldest_pending_age_ms',CASE WHEN min(created_at_ms) FILTER (WHERE state IN ('pending','leased'))
       IS NOT NULL THEN greatest(0,$2::bigint-min(created_at_ms) FILTER (WHERE state IN ('pending','leased'))) END)
FROM relationship_outbox WHERE scope_id=$1
)sql",
                                         std::array{scope_, std::to_string(now)});
            auto result = meowlive::protocol::json::parse_object(rows.at(0).at(0).value());
            if (!result)
            {
                throw std::runtime_error("graph_projection_status_invalid");
            }
            return std::move(*result);
        }

        std::optional<std::uint64_t> rebuild(const std::string& key, const std::string& reason,
                                             std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            db.execute("BEGIN");
            db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                       std::array{scope_});
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope_});
            const std::array args{scope_, key, reason, std::to_string(now)};
            const auto prior = db.execute("SELECT reason,result,fingerprint FROM relationship_audit"
                                          " WHERE scope_id=$1 AND request_key=$2",
                                          std::array{scope_, key});
            if (!prior.empty())
            {
                const auto& row = prior.at(0);
                if (row.at(0) != reason || !row.at(2).value().starts_with("rebuild:"))
                {
                    return {};
                }
                return std::stoull(row.at(1).value());
            }
            const auto rows = db.execute(R"sql(
WITH queued AS (
 INSERT INTO relationship_outbox(scope_id,fact_id,version,available_at_ms,created_at_ms)
 SELECT scope_id,id,version,$4::bigint,$4::bigint FROM relationship_facts WHERE scope_id=$1
 ON CONFLICT(scope_id,fact_id) DO UPDATE SET version=EXCLUDED.version,state='pending',attempts=0,
   available_at_ms=EXCLUDED.available_at_ms,created_at_ms=EXCLUDED.created_at_ms,token=NULL,lease_until_ms=NULL
 RETURNING fact_id
)
INSERT INTO relationship_audit(scope_id,request_key,reason,actor,created_at_ms,fingerprint,result)
SELECT $1,$2,$3,'single_admin',$4::bigint,'rebuild:'||to_jsonb($3::text)::text,to_jsonb(count(*)) FROM queued
RETURNING result
)sql",
                                         args);
            db.execute("COMMIT");
            return std::stoull(rows.at(0).at(0).value());
        }

    private:
        void prepare(meowlive::adapters::storage::PostgresConnection& db)
        {
            std::call_once(migrated_,
                           [&]
                           {
                               meowlive::adapters::storage::migrate_postgres(db, migrations_);
                           });
            db.execute("SET statement_timeout='5s'");
        }

        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        std::once_flag migrated_;
    };
}
