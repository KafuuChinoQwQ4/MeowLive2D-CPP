module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.viewer_merge_data;

import meowlive.standard;

import meowlive.adapters.storage.postgres;

export namespace meowlive::adapters::storage
{
    void move_viewer_data(meowlive::adapters::storage::PostgresConnection& db,
                          const std::string& scope, const std::string& source,
                          const std::string& target, const std::string& now, const std::string& key,
                          const std::string& reason, const std::string& fingerprint,
                          int familiarity, int affinity, int familiarity_delta, int affinity_delta)
    {
        const std::array viewers{scope, source, target};
        db.execute(R"sql(
UPDATE affinity_ledger SET original_viewer_id=COALESCE(original_viewer_id,viewer_id)
WHERE scope_id=$1 AND viewer_id IN ($2::uuid,$3::uuid)
)sql",
                   viewers);
        for (std::string_view table :
             {"viewer_identities", "viewer_events", "gift_ledger", "affinity_ledger",
              "companionship_reply_events", "companionship_completed_events"})
        {
            db.execute("UPDATE " + std::string(table) +
                           " SET viewer_id=$3::uuid WHERE scope_id=$1 AND viewer_id=$2::uuid",
                       viewers);
        }
        db.execute(R"sql(
INSERT INTO viewer_aliases(scope_id,viewer_id,alias,first_seen_at_ms,last_seen_at_ms)
SELECT scope_id,$3::uuid,alias,first_seen_at_ms,last_seen_at_ms FROM viewer_aliases
WHERE scope_id=$1 AND viewer_id=$2::uuid
ON CONFLICT(scope_id,viewer_id,alias) DO UPDATE
SET first_seen_at_ms=LEAST(viewer_aliases.first_seen_at_ms,EXCLUDED.first_seen_at_ms),
    last_seen_at_ms=GREATEST(viewer_aliases.last_seen_at_ms,EXCLUDED.last_seen_at_ms)
)sql",
                   viewers);
        db.execute("DELETE FROM viewer_aliases WHERE scope_id=$1 AND viewer_id=$2::uuid",
                   std::array{scope, source});
        constexpr std::array dedup_tables{std::pair{"viewer_presence", "day"},
                                          std::pair{"viewer_observed_sessions", "session_id"},
                                          std::pair{"companionship_daily_chats", "day,body_hash"},
                                          std::pair{"memory_suppressions", "key,source,event_id"}};
        for (const auto& [table, fields] : dedup_tables)
        {
            db.execute(std::string("INSERT INTO ") + table + "(scope_id,viewer_id," + fields +
                           ") SELECT scope_id,$3::uuid," + fields + " FROM " + table +
                           " WHERE scope_id=$1 AND viewer_id=$2::uuid ON CONFLICT DO NOTHING",
                       viewers);
            db.execute(std::string("DELETE FROM ") + table +
                           " WHERE scope_id=$1 AND viewer_id=$2::uuid",
                       std::array{scope, source});
        }
        db.execute(R"sql(
INSERT INTO viewer_companionship(scope_id,viewer_id,last_seen_at_ms)
VALUES($1,$2::uuid,$3::bigint) ON CONFLICT DO NOTHING
)sql",
                   std::array{scope, target, now});
        db.execute(R"sql(
UPDATE viewer_companionship
SET familiarity_milli=$4::int,affinity_milli=$5::int,
    last_seen_at_ms=GREATEST(last_seen_at_ms,COALESCE((SELECT last_seen_at_ms FROM viewer_companionship
        WHERE scope_id=$1 AND viewer_id=$2::uuid),0))
WHERE scope_id=$1 AND viewer_id=$3::uuid
)sql",
                   std::array{scope, source, target, std::to_string(familiarity),
                              std::to_string(affinity)});
        db.execute("UPDATE viewer_companionship SET familiarity_milli=0,affinity_milli=0"
                   " WHERE scope_id=$1 AND viewer_id=$2::uuid",
                   std::array{scope, source});
        db.execute(R"sql(
INSERT INTO affinity_ledger(id,scope_id,viewer_id,kind,day,computed_delta_milli,applied_delta_milli,
                            reason,actor,created_at_ms,request_key,fingerprint,original_viewer_id)
SELECT gen_random_uuid(),$1,$2::uuid,kind,$3::bigint / 86400000,delta,delta,
       $5,'single_admin',$3::bigint,'merge:'||kind||':'||$4,$6,$2::uuid
FROM (VALUES ('merge_familiarity',$7::int), ('merge_affinity',$8::int)) AS adjustments(kind,delta)
)sql",
                   std::array{scope, target, now, key, reason, fingerprint,
                              std::to_string(familiarity_delta), std::to_string(affinity_delta)});

        const std::array timed_viewers{scope, source, target, now};
        db.execute(R"sql(
UPDATE memories SET locked=true,status=CASE WHEN deleted THEN status ELSE 'candidate' END,
    expires_at_ms=CASE WHEN deleted THEN expires_at_ms ELSE
         LEAST(COALESCE(expires_at_ms,$4::bigint+604800000),$4::bigint+604800000) END,
    version=version+1,updated_at_ms=$4::bigint
WHERE scope_id=$1 AND viewer_id IN ($2::uuid,$3::uuid)
  AND key IN (SELECT key FROM memories WHERE scope_id=$1 AND viewer_id IN ($2::uuid,$3::uuid)
              GROUP BY key HAVING count(DISTINCT viewer_id)>1)
)sql",
                   timed_viewers);
        for (std::string_view table : {"memory_vectors", "memory_embedding_jobs"})
        {
            db.execute("DELETE FROM " + std::string(table) +
                           " v USING memories m WHERE v.scope_id=$1 AND m.scope_id=v.scope_id AND"
                           " m.id=v.memory_id AND m.viewer_id IN ($2::uuid,$3::uuid)",
                       viewers);
        }
        db.execute(R"sql(
UPDATE memories SET viewer_id=$3::uuid,version=version+1,updated_at_ms=$4::bigint
WHERE scope_id=$1 AND viewer_id=$2::uuid
)sql",
                   timed_viewers);
        db.execute(R"sql(
UPDATE memory_jobs SET viewer_id=$3::uuid,
    state=CASE WHEN state='running' THEN CASE WHEN attempts>=3 THEN 'failed' ELSE 'pending' END ELSE state END,
    token=NULL,lease_until_ms=NULL,available_at_ms=$4::bigint
WHERE scope_id=$1 AND viewer_id IN ($2::uuid,$3::uuid)
)sql",
                   timed_viewers);
        db.execute(R"sql(
WITH changed AS (
    UPDATE relationship_facts SET
      source_id=CASE WHEN source_kind='viewer' AND source_id=$2 THEN $3 ELSE source_id END,
      target_id=CASE WHEN target_kind='viewer' AND target_id=$2 THEN $3 ELSE target_id END,
      deleted=deleted OR (source_kind=target_kind AND (
        CASE WHEN source_kind='viewer' AND source_id=$2 THEN $3 ELSE source_id END
        = CASE WHEN target_kind='viewer' AND target_id=$2 THEN $3 ELSE target_id END
      )),
      confirmation='claimed',version=version+1,updated_at_ms=$4::bigint
    WHERE scope_id=$1 AND ((source_kind='viewer' AND source_id IN($2,$3)) OR
                           (target_kind='viewer' AND target_id IN($2,$3)))
    RETURNING id,version
)
INSERT INTO relationship_outbox(scope_id,fact_id,version,available_at_ms,created_at_ms)
SELECT $1,id,version,$4::bigint,$4::bigint FROM changed
ON CONFLICT(scope_id,fact_id) DO UPDATE SET version=EXCLUDED.version,
state='pending',attempts=0,available_at_ms=EXCLUDED.available_at_ms,token=NULL,lease_until_ms=NULL
)sql",
                   timed_viewers);
        db.execute(R"sql(
UPDATE viewers SET merged_into=$3::uuid WHERE scope_id=$1 AND (id=$2::uuid OR merged_into=$2::uuid)
)sql",
                   viewers);
    }
}
