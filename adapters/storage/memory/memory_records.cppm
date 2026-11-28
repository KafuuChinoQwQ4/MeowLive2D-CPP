module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.memory_records;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.relationship_derivation;
import meowlive.application.memory_extraction;

namespace
{
    using Connection = meowlive::adapters::storage::PostgresConnection;
    using Source = meowlive::application::MemorySource;
    using Candidate = meowlive::application::MemoryCandidate;

    void load_previous_evidence(Connection& db, const std::string& scope, const std::string& id,
                                const Source& source, Candidate& candidate,
                                std::vector<Source>& sources)
    {
        const auto rows = db.execute(
            R"sql(
SELECT proof.source,proof.event_id,proof.quote,proof.occurred_at_ms,proof.day,event.payload->>'text'
FROM memory_evidence proof
JOIN viewer_events event USING(scope_id,source,event_id)
WHERE proof.scope_id=$1 AND proof.memory_id=$2::uuid AND event.viewer_id=$3::uuid
    AND (proof.source,proof.event_id) <> ($4,$5) AND event.payload->>'text'<>''
    AND NOT EXISTS (SELECT FROM memory_suppressions x WHERE x.scope_id=$1
        AND x.viewer_id=$3::uuid AND x.key=$6 AND x.source=proof.source AND x.event_id=proof.event_id)
ORDER BY proof.occurred_at_ms,proof.source,proof.event_id LIMIT 31
)sql",
            std::array{scope, id, source.viewer_id, source.source, source.event_id, candidate.key});
        for (const auto& row : rows)
        {
            const auto& quote = row[2].value();
            if (quote.find(candidate.value) == std::string::npos ||
                row[5]->find(quote) == std::string::npos)
            {
                continue;
            }
            const auto occurred = std::stoll(row[3].value());
            const auto day = std::stoll(row[4].value());
            sources.push_back(
                {row[0].value(), source.viewer_id, row[1].value(), row[5].value(), occurred, day});
            candidate.evidence.push_back({row[0].value(), row[1].value(), quote, occurred, day});
        }
    }
}

export namespace meowlive::adapters::storage
{
    void merge_extracted_memory(Connection& db, const std::string& scope, const Source& source,
                                Candidate candidate, std::int64_t now)
    {
        const auto blocked = db.execute(
            R"sql(
SELECT EXISTS(SELECT FROM memory_suppressions WHERE scope_id=$1 AND viewer_id=$2::uuid
    AND key=$3 AND ((source='' AND event_id='') OR (source=$4 AND event_id=$5)))
    OR EXISTS(SELECT FROM memories WHERE scope_id=$1 AND viewer_id=$2::uuid AND key=$3 AND locked)
)sql",
            std::array{scope, source.viewer_id, candidate.key, source.source, source.event_id});
        if (blocked[0][0].value() == "t")
        {
            return;
        }
        const auto existing =
            db.execute(R"sql(
SELECT id,expires_at_ms, EXISTS(SELECT FROM memory_evidence proof
    WHERE proof.scope_id=$1 AND proof.memory_id=memories.id AND proof.source=$5 AND proof.event_id=$6)
FROM memories WHERE scope_id=$1 AND viewer_id=$2::uuid AND key=$3 AND value=$4 AND NOT deleted
ORDER BY updated_at_ms DESC,id LIMIT 1 FOR UPDATE
)sql",
                       std::array{scope, source.viewer_id, candidate.key, candidate.value,
                                  source.source, source.event_id});
        const bool direct = candidate.status == "long_term";
        const auto timestamp = std::to_string(now);
        std::string id;
        if (!existing.empty())
        {
            if (existing[0][2].value() == "t")
            {
                return;
            }
            id = existing[0][0].value();
            std::vector sources{source};
            load_previous_evidence(db, scope, id, source, candidate, sources);
            if (!meowlive::application::assess_memory(candidate, sources, now))
            {
                throw std::runtime_error("memory_evidence_invalid");
            }
            if (candidate.status != "long_term" && existing[0][1])
            {
                const auto old = static_cast<std::int64_t>(std::stoll(*existing[0][1]));
                if (!candidate.expires_at_ms || *candidate.expires_at_ms > old)
                {
                    candidate.expires_at_ms = old;
                    if (old <= now)
                    {
                        candidate.status = "expired";
                    }
                }
            }
        }
        const auto occurred = std::to_string(source.occurred_at_ms);
        if (candidate.status == "long_term" &&
            (candidate.kind == "preference" || candidate.kind == "preferred_name"))
        {
            const auto newer = db.execute(
                R"sql(
SELECT EXISTS(SELECT FROM memories old JOIN memory_evidence proof
    ON proof.scope_id=old.scope_id AND proof.memory_id=old.id
    WHERE old.scope_id=$1 AND old.viewer_id=$2::uuid AND old.key=$3
        AND old.value<>$4 AND NOT old.deleted AND proof.occurred_at_ms>$5::bigint)
)sql",
                std::array{scope, source.viewer_id, candidate.key, candidate.value, occurred});
            if (newer[0][0].value() == "t")
            {
                candidate.expires_at_ms = source.occurred_at_ms + 7 * 86'400'000LL;
                candidate.status = *candidate.expires_at_ms <= now ? "expired" : "candidate";
            }
            else if (direct)
            {
                db.execute(
                    R"sql(
UPDATE memories SET deleted=true,version=version+1,updated_at_ms=$5::bigint
WHERE scope_id=$1 AND viewer_id=$2::uuid AND key=$3 AND value<>$4 AND NOT deleted AND NOT locked
)sql",
                    std::array{scope, source.viewer_id, candidate.key, candidate.value, timestamp});
            }
        }

        const auto expiry = candidate.expires_at_ms ? std::to_string(*candidate.expires_at_ms) : "";
        const auto rows =
            db.execute(R"sql(
INSERT INTO memories(id,scope_id,viewer_id,key,value,kind,status,created_at_ms,updated_at_ms,expires_at_ms)
VALUES(COALESCE(NULLIF($1,'')::uuid,gen_random_uuid()),$2,$3::uuid,$4,$5,$6,$7,
    $8::bigint,$8::bigint,NULLIF($9,'')::bigint)
ON CONFLICT(id) DO UPDATE SET status=excluded.status,version=memories.version+1,
    expires_at_ms=excluded.expires_at_ms,updated_at_ms=excluded.updated_at_ms
RETURNING id
)sql",
                       std::array{id, scope, source.viewer_id, candidate.key, candidate.value,
                                  candidate.kind, candidate.status, timestamp, expiry});
        id = rows[0][0].value();
        for (const auto& proof : candidate.evidence)
        {
            db.execute(R"sql(
INSERT INTO memory_evidence(scope_id,memory_id,source,event_id,quote,occurred_at_ms,day)
VALUES($1,$2::uuid,$3,$4,$5,$6::bigint,$7::bigint) ON CONFLICT DO NOTHING
)sql",
                       std::array{scope, id, proof.source, proof.event_id, proof.quote,
                                  std::to_string(proof.occurred_at_ms), std::to_string(proof.day)});
        }
        meowlive::adapters::storage::derive_memory_relationship(db, scope, id,
                                                                static_cast<std::uint64_t>(now));
    }

    void prune_extracted_memories(Connection& db, const std::string& scope,
                                  const std::string& viewer, const std::string& now)
    {
        db.execute(R"sql(
UPDATE memories SET deleted=true,version=version+1
WHERE scope_id=$1 AND id IN (
    SELECT id FROM (
        SELECT id,status,row_number() OVER(PARTITION BY status ORDER BY updated_at_ms DESC,id) AS ordinal
        FROM memories WHERE scope_id=$1 AND viewer_id=$2::uuid AND NOT deleted AND NOT locked
            AND status IN('candidate','short_term')
    ) ranked WHERE ordinal>CASE WHEN status='candidate' THEN 50 ELSE 200 END
)
)sql",
                   std::array{scope, viewer});
        db.execute(R"sql(
DELETE FROM memory_vectors v USING memories m WHERE v.scope_id=$1 AND m.scope_id=v.scope_id
    AND m.id=v.memory_id AND m.viewer_id=$2::uuid AND (m.deleted OR v.version<>m.version)
)sql",
                   std::array{scope, viewer});
        db.execute(R"sql(
INSERT INTO memory_tombstones(scope_id,memory_id,version,created_at_ms)
SELECT scope_id,id,version,$3::bigint FROM memories WHERE scope_id=$1 AND viewer_id=$2::uuid AND deleted
ON CONFLICT(scope_id,memory_id) DO UPDATE SET version=excluded.version,created_at_ms=excluded.created_at_ms
WHERE memory_tombstones.version<excluded.version
)sql",
                   std::array{scope, viewer, now});
        db.execute("UPDATE memory_scopes SET revision=revision+1 WHERE scope_id=$1",
                   std::array{scope});
    }
}
