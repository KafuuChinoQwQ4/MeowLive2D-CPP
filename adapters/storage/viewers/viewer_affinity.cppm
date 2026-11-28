module;
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.viewer_affinity;

import meowlive.standard;
import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.domain.affinity;
import meowlive.protocol.json;

export namespace meowlive::adapters::storage
{
    class ViewerAffinity
    {
    public:
        ViewerAffinity(std::string url, std::filesystem::path libpq,
                       std::filesystem::path migrations, std::string scope, int offset)
            : url_(std::move(url)), libpq_(std::move(libpq)), migrations_(std::move(migrations)),
              scope_(std::move(scope)), offset_(std::to_string(offset))
        {
        }

        std::optional<::Json::Value> details(const std::string& viewer)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            const auto rows = db.execute(R"sql(
SELECT jsonb_build_object(
  'viewer_id', v.id,
  'affinity_milli', COALESCE(c.affinity_milli,0),
  'familiarity_milli', COALESCE(c.familiarity_milli,0),
  'last_seen_at_ms', COALESCE(c.last_seen_at_ms,v.alias_observed_at_ms,v.created_at_ms),
  'observed_days', (SELECT count(*) FROM viewer_presence p WHERE p.scope_id=v.scope_id AND p.viewer_id=v.id),
  'observed_sessions', (SELECT count(*) FROM viewer_observed_sessions s WHERE s.scope_id=v.scope_id AND s.viewer_id=v.id),
  'medal_level', c.medal_level, 'guard_level', c.guard_level,
  'gifts', COALESCE((
    SELECT jsonb_agg(to_jsonb(gift)) FROM (
      SELECT source,event_id,payload->'name' AS name,payload->'count' AS count,
             payload->'gift_metadata' AS metadata,value_cents,value_kind,occurred_at_ms
      FROM gift_ledger WHERE scope_id=v.scope_id AND viewer_id=v.id
      ORDER BY occurred_at_ms DESC,source,event_id LIMIT 100
    ) gift
  ), '[]'::jsonb),
  'ledger', COALESCE((
    SELECT jsonb_agg(to_jsonb(ledger)) FROM (
      SELECT l.id AS ledger_id,l.kind,l.computed_delta_milli,l.applied_delta_milli,l.reason,l.actor,
             l.created_at_ms,l.reversed_ledger_id,
             (l.original_viewer_id IS NULL AND l.kind IN ('manual','gift','exchange') AND
               NOT EXISTS(SELECT FROM affinity_ledger WHERE scope_id=l.scope_id AND reversed_ledger_id=l.id)) AS reversible
      FROM affinity_ledger l WHERE l.scope_id=v.scope_id AND l.viewer_id=v.id
      ORDER BY l.created_at_ms DESC,l.id LIMIT 100
    ) ledger
  ),'[]'::jsonb)
)
FROM viewers v LEFT JOIN viewer_companionship c ON c.scope_id=v.scope_id AND c.viewer_id=v.id
WHERE v.scope_id=$1 AND v.id=$2::uuid AND v.merged_into IS NULL
)sql",
                                         std::array{scope_, viewer});
            return rows.empty() ? std::nullopt
                                : meowlive::protocol::json::parse_object(rows.at(0).at(0).value());
        }

        std::optional<std::string> change(const std::string& viewer, const std::string& request_key,
                                          const std::string& reason, int delta,
                                          const std::string& reversed, std::uint64_t timestamp)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            db.execute("BEGIN");
            db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                       std::array{scope_});
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope_});
            if (db.execute("SELECT id FROM viewers WHERE scope_id=$1 AND id=$2::uuid "
                           "AND merged_into IS NULL FOR UPDATE",
                           std::array{scope_, viewer})
                    .empty())
            {
                return {};
            }

            const auto prior = existing(db, viewer, request_key, reason, delta, reversed);
            if (prior)
            {
                db.execute("COMMIT");
                return prior->empty() ? std::nullopt : prior;
            }

            const auto now = std::to_string(timestamp);
            if (!reversed.empty())
            {
                const auto origin = db.execute(R"sql(
SELECT l.applied_delta_milli FROM affinity_ledger l
WHERE l.scope_id=$1 AND l.viewer_id=$2::uuid AND l.id=$3::uuid
  AND l.kind IN ('manual','gift','exchange') AND l.original_viewer_id IS NULL
  AND NOT EXISTS(SELECT FROM affinity_ledger WHERE scope_id=l.scope_id AND reversed_ledger_id=l.id)
)sql",
                                               std::array{scope_, viewer, reversed});
                if (origin.empty())
                {
                    return {};
                }
                delta = -std::stoi(origin.at(0).at(0).value());
            }
            db.execute(R"sql(
INSERT INTO viewer_companionship(scope_id,viewer_id,last_seen_at_ms)
SELECT scope_id,id,COALESCE(alias_observed_at_ms,created_at_ms) FROM viewers WHERE scope_id=$1 AND id=$2::uuid
ON CONFLICT DO NOTHING
)sql",
                       std::array{scope_, viewer});
            const auto current =
                std::stoi(db.execute("SELECT affinity_milli FROM viewer_companionship WHERE "
                                     "scope_id=$1 AND viewer_id=$2::uuid FOR UPDATE",
                                     std::array{scope_, viewer})
                              .at(0)
                              .at(0)
                              .value());
            const auto next = meowlive::domain::apply_affinity_delta(current, delta);
            const auto kind = reversed.empty() ? "manual" : "reversal";
            const auto fingerprint =
                legacy_fingerprint(viewer, request_key, reason, delta, reversed);
            const auto added =
                db.execute(R"sql(
INSERT INTO affinity_ledger(id,scope_id,viewer_id,kind,day,computed_delta_milli,applied_delta_milli,
                            reason,actor,created_at_ms,request_key,fingerprint,reversed_ledger_id)
VALUES(gen_random_uuid(),$1,$2::uuid,$3,
       floor(($4::numeric+$5::int*60000)/86400000)::bigint,
       $6::int,$7::int,$8,'single_admin',$4::bigint,'admin:'||$9,$10,NULLIF($11,'')::uuid)
RETURNING id
)sql",
                           std::array{scope_, viewer, std::string(kind), now, offset_,
                                      std::to_string(delta), std::to_string(next - current), reason,
                                      request_key, fingerprint, reversed});
            db.execute("UPDATE viewer_companionship SET affinity_milli=$3::int "
                       "WHERE scope_id=$1 AND viewer_id=$2::uuid",
                       std::array{scope_, viewer, std::to_string(next)});
            db.execute("COMMIT");
            return added.at(0).at(0);
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

        static std::string legacy_fingerprint(const std::string& viewer, const std::string& key,
                                              const std::string& reason, int delta,
                                              const std::string& reversed)
        {
            const auto encoded_key = meowlive::protocol::json::serialize(::Json::Value(key));
            const auto encoded_reason = meowlive::protocol::json::serialize(::Json::Value(reason));
            if (reversed.empty())
            {
                return "adjust:" + viewer + ":AffinityAdjustment { request_key: " + encoded_key +
                       ", reason: " + encoded_reason + ", delta_milli: " + std::to_string(delta) +
                       " }";
            }
            return "reverse:" + viewer + ":AffinityReversal { request_key: " + encoded_key +
                   ", reason: " + encoded_reason +
                   ", ledger_id: " + meowlive::protocol::json::serialize(::Json::Value(reversed)) +
                   " }";
        }

        std::optional<std::string> existing(meowlive::adapters::storage::PostgresConnection& db,
                                            const std::string& viewer, const std::string& key,
                                            const std::string& reason, int delta,
                                            const std::string& reversed) const
        {
            const auto rows = db.execute(
                R"sql(
SELECT id,
       viewer_id=$2::uuid AND reason=$3 AND original_viewer_id IS NULL AND
       CASE WHEN $4='' THEN kind='manual' AND computed_delta_milli=$5::int
         ELSE kind='reversal' AND reversed_ledger_id=$4::uuid END
FROM affinity_ledger WHERE scope_id=$1 AND request_key='admin:'||$6
)sql",
                std::array{scope_, viewer, reason, reversed, std::to_string(delta), key});
            if (rows.empty())
            {
                return {};
            }
            return rows.at(0).at(1) == "t" ? rows.at(0).at(0).value() : std::string{};
        }

        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        const std::string offset_;
        std::once_flag migrated_;
    };
}
