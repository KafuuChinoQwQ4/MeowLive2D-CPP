module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.viewer_merge;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.adapters.storage.viewer_merge_data;
import meowlive.protocol.json;

namespace
{
    using Connection = meowlive::adapters::storage::PostgresConnection;

    std::int64_t lock_scope(Connection& db, const std::string& scope)
    {
        db.execute("BEGIN");
        db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                   std::array{scope});
        return std::stoll(
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope})
                .at(0)
                .at(0)
                .value());
    }

    std::optional<::Json::Value> summary(Connection& db, const std::string& scope,
                                         const std::string& viewer)
    {
        const auto rows = db.execute(R"sql(
SELECT jsonb_build_object('viewer_id',v.id,'alias',v.current_alias,
    'identities',(SELECT count(*) FROM viewer_identities WHERE scope_id=v.scope_id AND viewer_id=v.id),
    'events',(SELECT count(*) FROM viewer_events WHERE scope_id=v.scope_id AND viewer_id=v.id),
    'memories',(SELECT count(*) FROM memories WHERE scope_id=v.scope_id AND viewer_id=v.id),
    'relationships',(SELECT count(*) FROM relationship_facts WHERE scope_id=v.scope_id AND
       ((source_kind='viewer' AND source_id=v.id::text) OR (target_kind='viewer' AND target_id=v.id::text))),
    'familiarity_milli',COALESCE(c.familiarity_milli,0),'affinity_milli',COALESCE(c.affinity_milli,0))
FROM viewers v LEFT JOIN viewer_companionship c ON c.scope_id=v.scope_id AND c.viewer_id=v.id
WHERE v.scope_id=$1 AND v.id=$2::uuid AND v.merged_into IS NULL
)sql",
                                     std::array{scope, viewer});
        if (rows.empty())
        {
            return {};
        }
        auto result = meowlive::protocol::json::parse_object(rows.at(0).at(0).value());
        return result && (*result)["identities"].asInt64() > 0 ? result : std::nullopt;
    }

    std::string hash(Connection& db, const std::string& payload)
    {
        return db.execute("SELECT encode(sha256(convert_to($1,'UTF8')),'hex')", std::array{payload})
            .at(0)
            .at(0)
            .value();
    }

    std::string preview_fingerprint(Connection& db, const std::string& scope,
                                    const std::string& source, const std::string& target,
                                    std::int64_t revision)
    {
        const std::array args{scope, source, target};
        std::string state = scope + ":" + source + ":" + target + ":" + std::to_string(revision);
        for (const std::string_view table :
             {"viewers", "viewer_identities", "viewer_events", "viewer_aliases",
              "viewer_companionship", "viewer_presence", "viewer_observed_sessions", "gift_ledger",
              "affinity_ledger", "companionship_reply_events", "companionship_completed_events",
              "companionship_daily_chats", "memories", "memory_jobs", "memory_suppressions"})
        {
            const auto column = table == "viewers" ? "id" : "viewer_id";
            const auto sql = "SELECT md5(COALESCE(string_agg(h,',' ORDER BY h),''))"
                             " FROM (SELECT md5(to_jsonb(t)::text) h FROM " +
                             std::string(table) + " t WHERE scope_id=$1 AND " + column +
                             " IN ($2::uuid,$3::uuid)) rows";
            state += table;
            state += db.execute(sql, args).at(0).at(0).value();
        }
        state += db.execute(R"sql(
SELECT md5(COALESCE(string_agg(h,',' ORDER BY h),'')) FROM (
 SELECT md5(to_jsonb(t)::text) h FROM relationship_facts t WHERE scope_id=$1 AND (
   (source_kind='viewer' AND source_id IN($2,$3)) OR (target_kind='viewer' AND target_id IN($2,$3))
 )
) rows
)sql",
                            args)
                     .at(0)
                     .at(0)
                     .value();
        return hash(db, state);
    }

    std::optional<::Json::Value> inspect(Connection& db, const std::string& scope,
                                         const std::string& source, const std::string& target,
                                         std::int64_t revision)
    {
        const std::array args{scope, source, target};
        if (db.execute("SELECT id FROM viewers WHERE scope_id=$1 AND id IN ($2::uuid,$3::uuid)"
                       " AND merged_into IS NULL ORDER BY id FOR UPDATE",
                       args)
                .size() != 2)
        {
            return {};
        }
        auto from = summary(db, scope, source);
        auto into = summary(db, scope, target);
        if (!from || !into)
        {
            return {};
        }
        const auto days =
            db.execute("SELECT LEAST(100,COUNT(DISTINCT day))::int FROM viewer_presence"
                       " WHERE scope_id=$1 AND viewer_id IN ($2::uuid,$3::uuid)",
                       args);
        ::Json::Value result(::Json::objectValue);
        result["resulting_affinity_milli"] =
            std::max((*from)["affinity_milli"].asInt(), (*into)["affinity_milli"].asInt());
        result["resulting_familiarity_milli"] = 1000 * std::stoi(days.at(0).at(0).value());
        result["source"] = std::move(*from);
        result["target"] = std::move(*into);
        result["revision"] = static_cast<::Json::Int64>(revision);
        result["fingerprint"] = preview_fingerprint(db, scope, source, target, revision);
        auto& risks = result["risks"] = ::Json::Value(::Json::arrayValue);
        risks.append("熟悉度按来访日并集计分，好感度取较高值。原流水保留来源。");
        risks.append("重复记忆键冻结，关联关系须重新确认。");
        risks.append("源档案永久指向目标，源身份此后归入目标档案。");
        return result;
    }

    ::Json::Value merge_result(const std::string& target, std::int64_t revision)
    {
        ::Json::Value result(::Json::objectValue);
        result["canonical_viewer_id"] = target;
        result["revision"] = static_cast<::Json::Int64>(revision);
        return result;
    }
}

export namespace meowlive::adapters::storage
{
    struct ViewerMergeRequest
    {
        std::string source;
        std::string target;
        std::int64_t revision{};
        std::string fingerprint;
        std::string key;
        std::string reason;
    };

    class ViewerMerge
    {
    public:
        ViewerMerge(std::string url, std::filesystem::path libpq, std::filesystem::path migrations,
                    std::string scope)
            : url_(std::move(url)), libpq_(std::move(libpq)), migrations_(std::move(migrations)),
              scope_(std::move(scope))
        {
        }

        std::optional<::Json::Value> preview(const std::string& source, const std::string& target)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            auto result = inspect(db, scope_, source, target, lock_scope(db, scope_));
            db.execute("ROLLBACK");
            return result;
        }

        std::optional<::Json::Value>
        apply(const meowlive::adapters::storage::ViewerMergeRequest& request,
              std::uint64_t timestamp)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            prepare(db);
            const auto revision = lock_scope(db, scope_);
            ::Json::Value fields(::Json::arrayValue);
            fields.append(request.source);
            fields.append(request.target);
            fields.append(static_cast<::Json::Int64>(request.revision));
            fields.append(request.fingerprint);
            fields.append(request.key);
            fields.append(request.reason);
            const auto request_hash = hash(db, meowlive::protocol::json::serialize(fields));
            const auto existing =
                db.execute("SELECT target_viewer_id,result_revision,request_fingerprint FROM "
                           "viewer_merge_audit"
                           " WHERE scope_id=$1 AND request_key=$2",
                           std::array{scope_, request.key});
            if (!existing.empty())
            {
                const auto& row = existing.at(0);
                return row[2] == request_hash
                           ? std::optional(merge_result(row[0].value(), std::stoll(row[1].value())))
                           : std::nullopt;
            }
            if (revision != request.revision)
            {
                return {};
            }
            const auto preview = inspect(db, scope_, request.source, request.target, revision);
            if (!preview || (*preview)["fingerprint"] != request.fingerprint)
            {
                return {};
            }
            const auto& source = (*preview)["source"];
            const auto& target = (*preview)["target"];
            const auto affinity = (*preview)["resulting_affinity_milli"].asInt();
            const auto familiarity = (*preview)["resulting_familiarity_milli"].asInt();
            const auto now = std::to_string(timestamp);
            meowlive::adapters::storage::move_viewer_data(
                db, scope_, request.source, request.target, now, request.key, request.reason,
                request.fingerprint, familiarity, affinity,
                familiarity - source["familiarity_milli"].asInt() -
                    target["familiarity_milli"].asInt(),
                affinity - source["affinity_milli"].asInt() - target["affinity_milli"].asInt());
            const auto new_revision = db.execute("UPDATE memory_scopes SET revision=revision+1"
                                                 " WHERE scope_id=$1 RETURNING revision",
                                                 std::array{scope_})
                                          .at(0)
                                          .at(0)
                                          .value();
            db.execute(R"sql(
INSERT INTO viewer_merge_audit(scope_id,request_key,source_viewer_id,target_viewer_id,
    expected_revision,preview_fingerprint,request_fingerprint,reason,actor,created_at_ms,result_revision)
VALUES($1,$2,$3::uuid,$4::uuid,$5::bigint,$6,$7,$8,'single_admin',$9::bigint,$10::bigint)
)sql",
                       std::array{scope_, request.key, request.source, request.target,
                                  std::to_string(revision), request.fingerprint, request_hash,
                                  request.reason, now, new_revision});
            db.execute("COMMIT");
            return merge_result(request.target, std::stoll(new_revision));
        }

    private:
        void prepare(meowlive::adapters::storage::PostgresConnection& db)
        {
            std::call_once(migrated_,
                           [&]
                           {
                               meowlive::adapters::storage::migrate_postgres(db, migrations_);
                           });
            db.execute("SET statement_timeout='10s'");
        }

        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        std::once_flag migrated_;
    };
}
