module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.memory_admin;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.protocol.json;

export namespace meowlive::adapters::storage
{
    class MemoryAdmin
    {
    public:
        MemoryAdmin(std::string url, std::filesystem::path library,
                    std::filesystem::path migrations, std::string scope, bool vectors_enabled)
            : url_(std::move(url)), library_(std::move(library)),
              migrations_(std::move(migrations)), scope_(std::move(scope)),
              vectors_enabled_(vectors_enabled)
        {
        }

        ::Json::Value list(const std::string& viewer, std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            prepare(db);
            return json_result(db.execute(R"sql(
SELECT jsonb_build_object('memories', COALESCE(jsonb_agg(to_jsonb(page)), '[]'::jsonb))
FROM (
    SELECT m.id,m.viewer_id,m.key,m.value,m.kind,
       CASE WHEN m.expires_at_ms <= $3::bigint THEN 'expired' ELSE m.status END AS status,
       m.version,m.locked,m.deleted,m.expires_at_ms, COALESCE((
           SELECT jsonb_agg(to_jsonb(evidence))
           FROM (
               SELECT source,event_id,quote,occurred_at_ms FROM memory_evidence
               WHERE scope_id=m.scope_id AND memory_id=m.id
               ORDER BY occurred_at_ms,source,event_id LIMIT 32
           ) evidence
       ), '[]'::jsonb) AS evidence
    FROM memories m WHERE m.scope_id=$1 AND m.viewer_id=$2::uuid
    ORDER BY m.updated_at_ms DESC,m.id LIMIT 250
) page
)sql",
                                          std::array{scope_, viewer, std::to_string(now)}));
        }

        ::Json::Value status()
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            prepare(db);
            auto report = json_result(db.execute(R"sql(
SELECT jsonb_build_object(
    'pending', count(*) FILTER (WHERE state='pending'),
    'running', count(*) FILTER (WHERE state='running'),
    'failed', count(*) FILTER (WHERE state='failed'),
    'embedding_pending', (SELECT count(*) FROM memory_embedding_jobs
        WHERE scope_id=$1 AND attempts<3),
    'embedding_failed', (SELECT count(*) FROM memory_embedding_jobs
        WHERE scope_id=$1 AND attempts>=3))
FROM memory_jobs WHERE scope_id=$1
)sql",
                                                 std::array{scope_}));
            report["vectors_enabled"] = vectors_enabled_;
            return report;
        }

        bool vectors_enabled() const
        {
            return vectors_enabled_;
        }

        bool maintain(const std::string& key, const std::string& reason, bool rebuild,
                      std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            prepare(db);
            lock_scope(db);
            const auto operation = rebuild ? std::string("rebuild_vectors") : std::string("retry");
            const auto previous =
                db.execute("SELECT operation, reason, actor FROM memory_operations WHERE "
                           "scope_id=$1 AND request_key=$2",
                           std::array{scope_, key});
            if (!previous.empty())
            {
                return previous[0][0] == operation && previous[0][1] == reason &&
                       previous[0][2] == "single_admin";
            }
            const auto timestamp = std::to_string(now);
            db.execute(R"sql(
INSERT INTO memory_operations(scope_id,request_key,operation,reason,actor,created_at_ms)
 VALUES($1,$2,$3,$4,'single_admin',$5::bigint)
)sql",
                       std::array{scope_, key, operation, reason, timestamp});
            if (rebuild)
            {
                db.execute("DELETE FROM memory_vectors WHERE scope_id=$1", std::array{scope_});
                db.execute("DELETE FROM memory_embedding_jobs WHERE scope_id=$1",
                           std::array{scope_});
            }
            else
            {
                db.execute(R"sql(
UPDATE memory_jobs SET state='pending',attempts=0,available_at_ms=$2::bigint,
    token=NULL,lease_until_ms=NULL WHERE scope_id=$1 AND state='failed' AND body<>''
)sql",
                           std::array{scope_, timestamp});
                db.execute(R"sql(
DELETE FROM memory_embedding_jobs WHERE scope_id=$1 AND attempts>=3 AND lease_until_ms<=$2::bigint
)sql",
                           std::array{scope_, timestamp});
            }
            db.execute("COMMIT");
            return true;
        }

        bool mutate(const std::string& viewer, const std::string& memory,
                    const std::string& request, const std::string& reason,
                    const std::string& operation, const std::string& value, std::int64_t version,
                    std::uint64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            prepare(db);
            lock_scope(db);
            const auto version_string = std::to_string(version);
            const auto previous = db.execute(R"sql(
SELECT memory_id=$3::uuid AND viewer_id=$4::uuid AND operation=$5
    AND value IS NOT DISTINCT FROM NULLIF($6,'') AND reason=$7 AND actor='single_admin'
    AND expected_version=$8::bigint
FROM memory_audit WHERE scope_id=$1 AND request_key=$2
)sql",
                                             std::array{scope_, request, memory, viewer, operation,
                                                        value, reason, version_string});
            if (!previous.empty())
            {
                return previous[0][0] == "t";
            }
            const auto row = db.execute(R"sql(
SELECT key FROM memories WHERE scope_id=$1 AND viewer_id=$2::uuid AND id=$3::uuid
    AND version=$4::bigint AND NOT deleted FOR UPDATE
)sql",
                                        std::array{scope_, viewer, memory, version_string});
            if (row.empty())
            {
                return false;
            }
            const auto timestamp = std::to_string(now);
            db.execute(R"sql(
INSERT INTO memory_audit(scope_id,request_key,memory_id,viewer_id,operation,value,reason,
    actor,expected_version,created_at_ms)
VALUES($1,$2,$3::uuid,$4::uuid,$5,NULLIF($6,''),$7,'single_admin',$8::bigint,$9::bigint)
)sql",
                       std::array{scope_, request, memory, viewer, operation, value, reason,
                                  version_string, timestamp});
            db.execute(R"sql(
UPDATE memories SET value=COALESCE(NULLIF($4,''),value),deleted=($5='delete'),
    locked=($5<>'unfreeze'),status=CASE WHEN $5='correct' THEN 'long_term' ELSE status END,
    expires_at_ms=CASE WHEN $5='correct' THEN NULL ELSE expires_at_ms END,
    version=version+1,updated_at_ms=$3::bigint WHERE scope_id=$1 AND id=$2::uuid
)sql",
                       std::array{scope_, memory, timestamp, value, operation});
            db.execute("DELETE FROM memory_vectors WHERE scope_id=$1 AND memory_id=$2::uuid",
                       std::array{scope_, memory});
            db.execute("DELETE FROM memory_embedding_jobs WHERE scope_id=$1 AND memory_id=$2::uuid",
                       std::array{scope_, memory});
            const auto& key = row[0][0].value();
            if (operation == "unfreeze")
            {
                db.execute(R"sql(
DELETE FROM memory_suppressions WHERE scope_id=$1 AND viewer_id=$2::uuid AND key=$3
    AND source='' AND event_id=''
)sql",
                           std::array{scope_, viewer, key});
            }
            else if (operation == "delete" || operation == "correct")
            {
                db.execute(R"sql(
INSERT INTO memory_suppressions(scope_id,viewer_id,key,source,event_id)
SELECT $1,$2::uuid,$3,'',''
UNION ALL SELECT $1,$2::uuid,$3,source,event_id FROM memory_evidence
    WHERE scope_id=$1 AND memory_id=$4::uuid
ON CONFLICT DO NOTHING
)sql",
                           std::array{scope_, viewer, key, memory});
                db.execute(R"sql(
INSERT INTO memory_tombstones(scope_id,memory_id,version,created_at_ms)
VALUES($1,$2::uuid,$3::bigint+1,$4::bigint)
ON CONFLICT(scope_id,memory_id) DO UPDATE SET version=excluded.version,created_at_ms=excluded.created_at_ms
)sql",
                           std::array{scope_, memory, version_string, timestamp});
            }
            db.execute("UPDATE memory_scopes SET revision=revision+1 WHERE scope_id=$1",
                       std::array{scope_});
            db.execute("COMMIT");
            return true;
        }

    private:
        void prepare(meowlive::adapters::storage::PostgresConnection& db)
        {
            std::call_once(initialized_,
                           [&]
                           {
                               meowlive::adapters::storage::migrate_postgres(db, migrations_);
                           });
            db.execute("SET statement_timeout='5s'");
        }

        void lock_scope(meowlive::adapters::storage::PostgresConnection& db)
        {
            db.execute("BEGIN");
            db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                       std::array{scope_});
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope_});
        }

        static ::Json::Value json_result(const meowlive::adapters::storage::PostgresRows& rows)
        {
            auto result = meowlive::protocol::json::parse_object(rows.at(0).at(0).value());
            if (!result)
            {
                throw std::runtime_error("memory_query_result_invalid");
            }
            return std::move(*result);
        }

        const std::string url_;
        const std::filesystem::path library_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        const bool vectors_enabled_;
        std::once_flag initialized_;
    };
}
