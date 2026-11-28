module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.relationship_admin;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.protocol.json;

namespace
{
    using Connection = meowlive::adapters::storage::PostgresConnection;

    std::string relationship_json(const ::Json::Value& value)
    {
        return meowlive::protocol::json::serialize(value);
    }

    ::Json::Value relationship_record(const std::string& text)
    {
        const auto result = meowlive::protocol::json::parse_object(text);
        if (!result)
        {
            throw std::runtime_error("relationship_record_invalid");
        }
        return *result;
    }

    ::Json::Value relationship_dto(const ::Json::Value& record)
    {
        ::Json::Value result(::Json::objectValue);
        for (const auto* key :
             {"id", "version", "kind", "confirmation", "evidence", "expires_at_ms", "deleted"})
        {
            result[key] = record[key];
        }
        result["source"]["kind"] = record["source_kind"];
        result["source"]["id"] = record["source_id"];
        result["target"]["kind"] = record["target_kind"];
        result["target"]["id"] = record["target_id"];
        return result;
    }

    void lock_relationship_scope(Connection& db, const std::string& scope)
    {
        db.execute("BEGIN");
        db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                   std::array{scope});
        db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                   std::array{scope});
    }

    constexpr std::string_view rust_enum(std::string_view value)
    {
        constexpr std::array values{std::pair{"viewer", "Viewer"},
                                    std::pair{"topic", "Topic"},
                                    std::pair{"activity", "Activity"},
                                    std::pair{"unresolved", "Unresolved"},
                                    std::pair{"mention", "Mention"},
                                    std::pair{"acquaintance", "Acquaintance"},
                                    std::pair{"participated", "Participated"},
                                    std::pair{"shared_interest", "SharedInterest"},
                                    std::pair{"friend", "Friend"},
                                    std::pair{"confirm", "Confirm"},
                                    std::pair{"revoke", "Revoke"},
                                    std::pair{"delete", "Delete"}};
        for (const auto& [wire, legacy] : values)
        {
            if (wire == value)
            {
                return legacy;
            }
        }
        return {};
    }

    std::string relationship_fingerprint(const std::string& id, const ::Json::Value& request)
    {
        if (!id.empty())
        {
            return std::format("change:{}:{}:{}:{}", id, request["expected_version"].asInt64(),
                               rust_enum(request["action"].asString()),
                               relationship_json(request["reason"]));
        }
        // Preserve the stored fingerprint format used by the Rust administrator API.
        std::string evidence;
        for (const auto& proof : request["evidence"])
        {
            if (!evidence.empty())
            {
                evidence += ", ";
            }
            evidence += std::format("RelationEvidence {{ source: {}, event_id: {}, quote: {} }}",
                                    relationship_json(proof["source"]),
                                    relationship_json(proof["event_id"]),
                                    relationship_json(proof["quote"]));
        }
        const auto expiry = request["expires_at_ms"].isNull()
                                ? std::string("None")
                                : std::format("Some({})", request["expires_at_ms"].asInt64());
        return std::format(
            "create:RelationInput {{ source: RelationEntity {{ kind: {}, id: {} }}, "
            "target: RelationEntity {{ kind: {}, id: {} }}, kind: {}, evidence: [{}], "
            "expires_at_ms: {}, admin_confirmed: {}, reason: {}, request_key: {} }}",
            rust_enum(request["source"]["kind"].asString()),
            relationship_json(request["source"]["id"]),
            rust_enum(request["target"]["kind"].asString()),
            relationship_json(request["target"]["id"]), rust_enum(request["kind"].asString()),
            evidence, expiry, request["admin_confirmed"].asBool(),
            relationship_json(request["reason"]), relationship_json(request["request_key"]));
    }

    meowlive::adapters::storage::PostgresRows create_relationship(Connection& db,
                                                                  const std::string& scope,
                                                                  const ::Json::Value& request,
                                                                  const std::string& now)
    {
        return db.execute(
            R"sql(
WITH input AS (
  SELECT $2::text AS source_id,$3::text AS target_kind,$4::text AS target_id,
    $5::text AS kind,$6::jsonb AS evidence,NULLIF($7,'')::bigint AS expires,
    $8::bool AS confirmed,$9::bigint AS now
), supplied_proof AS (
  SELECT p.* FROM input, jsonb_to_recordset(evidence) p(source text,event_id text,quote text)
), matched_proof AS (
  SELECT e.viewer_id,e.source,e.event_id FROM supplied_proof p
  JOIN viewer_events e ON e.scope_id=$1 AND e.source=p.source AND e.event_id=p.event_id
  JOIN input i ON e.viewer_id::text IN (i.source_id,
                                      CASE WHEN i.target_kind='viewer' THEN i.target_id END)
  WHERE e.event_type='chat' AND strpos(e.payload->>'text',p.quote)>0
)
INSERT INTO relationship_facts(scope_id,id,source_kind,source_id,target_kind,target_id,kind,
                              confirmation,evidence,expires_at_ms,created_at_ms,updated_at_ms)
SELECT $1,gen_random_uuid(),'viewer',source_id,target_kind,target_id,kind,
       CASE WHEN confirmed THEN 'confirmed' ELSE 'claimed' END,evidence,expires,now,now
FROM input i
WHERE (expires IS NULL OR expires>now)
  AND EXISTS (SELECT FROM viewers WHERE scope_id=$1 AND id=i.source_id::uuid AND merged_into IS NULL)
  AND (target_kind<>'viewer' OR EXISTS (SELECT FROM viewers
       WHERE scope_id=$1 AND id=CASE WHEN i.target_kind='viewer' THEN i.target_id::uuid END
         AND merged_into IS NULL))
  AND (target_kind<>'viewer' OR target_id<>source_id)
  AND (SELECT count(*) FROM supplied_proof)=(SELECT count(DISTINCT (source,event_id)) FROM matched_proof)
  AND (confirmed OR EXISTS (SELECT FROM matched_proof WHERE viewer_id::text=i.source_id))
RETURNING to_jsonb(relationship_facts)
)sql",
            std::array{scope, request["source"]["id"].asString(),
                       request["target"]["kind"].asString(), request["target"]["id"].asString(),
                       request["kind"].asString(), relationship_json(request["evidence"]),
                       request["expires_at_ms"].isNull() ? std::string{}
                                                         : request["expires_at_ms"].asString(),
                       request["admin_confirmed"].asString(), now});
    }
}

export namespace meowlive::adapters::storage
{
    std::optional<::Json::Value>
    edit_relationship(meowlive::adapters::storage::PostgresConnection& db, const std::string& scope,
                      const std::string& id, const ::Json::Value& request, std::uint64_t now)
    {
        const auto key = request["request_key"].asString();
        const auto fingerprint = relationship_fingerprint(id, request);
        lock_relationship_scope(db, scope);
        const auto prior = db.execute("SELECT fingerprint,result FROM relationship_audit WHERE "
                                      "scope_id=$1 AND request_key=$2",
                                      std::array{scope, key});
        if (!prior.empty())
        {
            db.execute("ROLLBACK");
            return prior[0][0] == fingerprint
                       ? std::optional(relationship_dto(relationship_record(prior[0][1].value())))
                       : std::nullopt;
        }
        const auto clock = std::to_string(now);
        const auto rows =
            id.empty() ? create_relationship(db, scope, request, clock)
                       : db.execute(R"sql(
UPDATE relationship_facts SET version=version+1,updated_at_ms=$5::bigint,
 confirmation=CASE $4 WHEN 'confirm' THEN 'confirmed' WHEN 'revoke' THEN 'claimed' ELSE confirmation END,
 deleted=($4='delete')
WHERE scope_id=$1 AND id=$2::uuid AND version=$3::bigint AND NOT deleted
RETURNING to_jsonb(relationship_facts)
)sql",
                                    std::array{scope, id, request["expected_version"].asString(),
                                               request["action"].asString(), clock});
        if (rows.empty())
        {
            db.execute("ROLLBACK");
            return {};
        }
        const auto record = relationship_record(rows[0][0].value());
        const auto fact_id = record["id"].asString();
        db.execute(R"sql(
INSERT INTO relationship_outbox(scope_id,fact_id,version,available_at_ms,created_at_ms)
VALUES($1,$2::uuid,$3::bigint,$4::bigint,$4::bigint)
ON CONFLICT(scope_id,fact_id) DO UPDATE SET version=EXCLUDED.version,state='pending',attempts=0,
 available_at_ms=EXCLUDED.available_at_ms,created_at_ms=EXCLUDED.created_at_ms,token=NULL,lease_until_ms=NULL
)sql",
                   std::array{scope, fact_id, record["version"].asString(), clock});
        db.execute(R"sql(
INSERT INTO relationship_audit(scope_id,request_key,fingerprint,fact_id,reason,created_at_ms,result)
VALUES($1,$2,$3,$4::uuid,$5,$6::bigint,$7::jsonb)
)sql",
                   std::array{scope, key, fingerprint, fact_id, request["reason"].asString(), clock,
                              relationship_json(record)});
        db.execute("UPDATE memory_scopes SET revision=revision+1 WHERE scope_id=$1",
                   std::array{scope});
        db.execute("COMMIT");
        return relationship_dto(record);
    }
}
