module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.relationship_queries;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.protocol.json;

export namespace meowlive::adapters::storage
{
    ::Json::Value
    list_viewer_relationships(meowlive::adapters::storage::PostgresConnection& connection,
                              const std::string& scope, const std::string& viewer,
                              const ::Json::Value& references, std::uint64_t now)
    {
        const auto rows = connection.execute(
            R"sql(
WITH direct AS MATERIALIZED (
  SELECT f.* FROM relationship_facts f
  WHERE scope_id=$1 AND ((source_kind='viewer' AND source_id=$2)
      OR (target_kind='viewer' AND target_id=$2))
  ORDER BY updated_at_ms DESC,id LIMIT 100
), reachable AS (
  SELECT DISTINCT CASE WHEN source_kind='viewer' AND source_id=$2 THEN target_kind
                       ELSE source_kind END AS kind,
                  CASE WHEN source_kind='viewer' AND source_id=$2 THEN target_id
                       ELSE source_id END AS id
  FROM direct f
  WHERE NOT f.deleted AND f.confirmation='confirmed'
    AND (f.expires_at_ms IS NULL OR f.expires_at_ms>$4::bigint)
    AND NOT EXISTS (
      SELECT FROM jsonb_array_elements(f.evidence) proof
      JOIN memory_evidence e ON e.scope_id=$1 AND e.source=proof->>'source'
        AND e.event_id=proof->>'event_id'
      JOIN memories m ON m.scope_id=e.scope_id AND m.id=e.memory_id
      WHERE m.deleted OR m.status='expired' OR m.expires_at_ms<=$4::bigint)
), secondary AS (
  SELECT DISTINCT f.* FROM jsonb_to_recordset($3::jsonb) ref(id text, version bigint)
  JOIN relationship_facts f ON f.scope_id=$1 AND f.id::text=ref.id AND f.version=ref.version
  WHERE NOT f.deleted AND f.confirmation='confirmed'
    AND (f.expires_at_ms IS NULL OR f.expires_at_ms>$4::bigint)
    AND NOT EXISTS (SELECT FROM direct WHERE id=f.id)
    AND EXISTS (SELECT FROM reachable r WHERE (r.kind,r.id)=(f.source_kind,f.source_id)
                                          OR (r.kind,r.id)=(f.target_kind,f.target_id))
    AND NOT EXISTS (
      SELECT FROM jsonb_array_elements(f.evidence) proof
      JOIN memory_evidence e ON e.scope_id=$1 AND e.source=proof->>'source'
        AND e.event_id=proof->>'event_id'
      JOIN memories m ON m.scope_id=e.scope_id AND m.id=e.memory_id
      WHERE m.deleted OR m.status='expired' OR m.expires_at_ms<=$4::bigint)
  ORDER BY f.updated_at_ms DESC,f.id LIMIT 40
)
SELECT jsonb_build_object('id',f.id,'version',f.version,
 'source',jsonb_build_object('kind',f.source_kind,'id',f.source_id),
 'target',jsonb_build_object('kind',f.target_kind,'id',f.target_id),
 'kind',f.kind,'confirmation',f.confirmation,'evidence',f.evidence,
 'expires_at_ms',f.expires_at_ms,'deleted',f.deleted)
FROM (SELECT 1 AS depth,d.* FROM direct d UNION ALL SELECT 2,s.* FROM secondary s) f
ORDER BY f.depth,f.updated_at_ms DESC,f.id
)sql",
            std::array{scope, viewer, meowlive::protocol::json::serialize(references),
                       std::to_string(now)});
        ::Json::Value result(::Json::arrayValue);
        for (const auto& row : rows)
        {
            const auto fact = meowlive::protocol::json::parse_object(row.at(0).value());
            if (!fact)
            {
                throw std::runtime_error("relationship_read_invalid");
            }
            result.append(*fact);
        }
        return result;
    }
}
