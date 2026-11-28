module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.relationship_derivation;

import meowlive.standard;

import meowlive.adapters.storage.postgres;

export namespace meowlive::adapters::storage
{
    void derive_memory_relationship(meowlive::adapters::storage::PostgresConnection& db,
                                    const std::string& scope, const std::string& memory_id,
                                    std::uint64_t now)
    {
        db.execute(R"sql(
WITH memory AS (
  SELECT m.*
  FROM memories m JOIN viewers viewer ON viewer.scope_id=m.scope_id AND viewer.id=m.viewer_id
  WHERE m.scope_id=$1 AND m.id=$2::uuid AND NOT m.deleted AND NOT m.locked
    AND viewer.merged_into IS NULL AND (m.expires_at_ms IS NULL OR m.expires_at_ms>$3::bigint)
    AND char_length(btrim(m.value)) BETWEEN 1 AND 128 AND m.value !~ '[[:cntrl:]]'
), evidence AS (
  SELECT e.source,e.event_id,e.quote
  FROM memory m
  JOIN memory_evidence e ON e.scope_id=m.scope_id AND e.memory_id=m.id
  JOIN viewer_events event ON event.scope_id=e.scope_id AND event.source=e.source
    AND event.event_id=e.event_id AND event.viewer_id=m.viewer_id
    AND event.event_type='chat' AND e.quote=event.payload->>'text'
    AND e.occurred_at_ms=event.occurred_at_ms
  WHERE char_length(e.quote)<=300 AND (
    (m.kind='preference' AND m.status='long_term'
        AND e.quote IN ('我喜欢'||m.value,'我最喜欢'||m.value)
        AND m.value !~ '(如果|假如|可能|也许|听说|开玩笑|骗|抑郁|疾病|政治|宗教|性取向|诊断|但|不|，|,|。|;|；)')
    OR (m.kind='experience' AND m.status='short_term'
        AND e.quote IN ('我参加了'||m.value,'我今天去了'||m.value,'我昨天去了'||m.value)
        AND m.value !~ '(如果|假如|可能|也许|听说|他说|她说|不是|不喜欢|讨厌|开玩笑|骗|抑郁|疾病|政治|宗教|性取向|诊断)')
    OR (m.kind='third_party_claim' AND m.status='candidate'
        AND e.quote IN ('我认识'||m.value,'我和'||m.value||'是朋友'))
  )
  ORDER BY e.occurred_at_ms,e.source,e.event_id LIMIT 8
), fact AS (
  INSERT INTO relationship_facts(scope_id,id,source_kind,source_id,target_kind,target_id,kind,
      confirmation,evidence,expires_at_ms,created_at_ms,updated_at_ms)
  SELECT m.scope_id,m.id,'viewer',m.viewer_id::text,
     CASE m.kind WHEN 'preference' THEN 'topic' WHEN 'experience' THEN 'activity' ELSE 'unresolved' END,
     m.value, CASE m.kind WHEN 'preference' THEN 'shared_interest' WHEN 'experience' THEN 'participated' ELSE 'mention' END,
     CASE WHEN m.kind='third_party_claim' THEN 'claimed' ELSE 'confirmed' END,
     (SELECT jsonb_agg(to_jsonb(e)) FROM evidence e),m.expires_at_ms,$3::bigint,$3::bigint
  FROM memory m WHERE EXISTS (SELECT FROM evidence)
  ON CONFLICT(scope_id,id) DO NOTHING RETURNING id
)
INSERT INTO relationship_outbox(scope_id,fact_id,version,available_at_ms,created_at_ms)
SELECT $1,id,1,$3::bigint,$3::bigint FROM fact ON CONFLICT DO NOTHING
)sql",
                   std::array{scope, memory_id, std::to_string(now)});
    }
}
