module;

#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.reply_ledger;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.application.reply_completion;
import meowlive.domain.affinity;
import meowlive.protocol.json;

namespace
{
    using Connection = meowlive::adapters::storage::PostgresConnection;

    ::Json::Value event_pair(const std::string& source, const std::string& id)
    {
        ::Json::Value row(::Json::arrayValue);
        row.append(source);
        row.append(id);
        return row;
    }

    struct RegisteredReply
    {
        bool inserted;
        bool completed;
    };

    RegisteredReply register_reply(Connection& db, const std::string& scope,
                                   const meowlive::application::ReplyCompletion& reply,
                                   const std::set<std::pair<std::string, std::string>>& events)
    {
        ::Json::Value keys(::Json::arrayValue);
        for (const auto& [source, id] : events)
        {
            keys.append(event_pair(source, id));
        }
        const auto serialized = meowlive::protocol::json::serialize(keys);
        const auto added = db.execute(
            "INSERT INTO companionship_replies(scope_id,speech_id,generated_at_ms,event_keys) "
            "VALUES($1,$2,$3::bigint,$4) ON CONFLICT DO NOTHING RETURNING speech_id",
            std::array{scope, reply.speech_id, std::to_string(reply.generated_at_ms), serialized});
        if (!added.empty())
        {
            return {true, false};
        }
        const auto prior =
            db.execute("SELECT event_keys, completed_at_ms FROM companionship_replies "
                       "WHERE scope_id=$1 AND speech_id=$2 FOR UPDATE",
                       std::array{scope, reply.speech_id});
        // JSONB comparison accepts any equivalent UTF-8 escaping from Rust and C++.
        const auto matches = db.execute("SELECT $1::jsonb = $2::jsonb",
                                        std::array{prior.at(0).at(0).value(), serialized});
        if (matches.at(0).at(0) != "t")
        {
            throw std::runtime_error("reply_completion_conflicting_events");
        }
        return {false, prior.front().at(1).has_value()};
    }

    void register_events(Connection& db, const std::string& scope, const std::string& speech_id,
                         const std::set<std::pair<std::string, std::string>>& events,
                         const std::string& calendar_offset)
    {
        for (const auto& [source, id] : events)
        {
            const auto found = db.execute(R"sql(
SELECT viewer_id, floor((occurred_at_ms::numeric + $4::int * 60000) / 86400000)::bigint,
       CASE WHEN event_type='chat' THEN payload->>'text' ELSE NULL END
FROM viewer_events WHERE scope_id=$1 AND source=$2 AND event_id=$3
)sql",
                                          std::array{scope, source, id, calendar_offset});
            if (found.empty())
            {
                throw std::runtime_error("reply_completion_event_unavailable");
            }
            const auto& row = found.front();
            if (!row[0])
            {
                continue;
            }
            const auto body =
                row[2] ? meowlive::domain::normalized_chat_body(*row[2]) : std::string{};
            db.execute(
                R"sql(
INSERT INTO companionship_reply_events(scope_id,speech_id,source,event_id,viewer_id,day,chat_body)
VALUES($1,$2,$3,$4,$5::uuid,$6::bigint,NULLIF($7,'')) ON CONFLICT DO NOTHING
)sql",
                std::array{scope, speech_id, source, id, row[0].value(), row[1].value(), body});
        }
    }

    void award_chat(Connection& db, const std::string& scope, const std::string& viewer,
                    const std::string& day, const std::string& key, const std::string& now,
                    bool first_for_viewer)
    {
        db.execute(R"sql(
INSERT INTO viewer_companionship(scope_id,viewer_id,last_seen_at_ms)
VALUES($1,$2::uuid,$3::bigint) ON CONFLICT DO NOTHING
)sql",
                   std::array{scope, viewer, now});
        const auto snapshot = db.execute(R"sql(
SELECT affinity_milli, (SELECT COALESCE(sum(computed_delta_milli),0)::bigint FROM affinity_ledger
                        WHERE scope_id=$1 AND viewer_id=$2::uuid AND day=$3::bigint AND kind='exchange')
FROM viewer_companionship WHERE scope_id=$1 AND viewer_id=$2::uuid FOR UPDATE
)sql",
                                         std::array{scope, viewer, day});
        const auto current = std::stoi(snapshot.at(0).at(0).value());
        const auto computed = meowlive::domain::exchange_delta(
            std::stoll(snapshot.front().at(1).value()), first_for_viewer);
        const auto next = meowlive::domain::apply_affinity_delta(current, computed);
        const auto added = db.execute(R"sql(
INSERT INTO affinity_ledger(id,scope_id,viewer_id,kind,day,computed_delta_milli,applied_delta_milli,
                            reason,actor,created_at_ms,request_key,fingerprint)
VALUES(gen_random_uuid(),$1,$2::uuid,'exchange',$3::bigint,$4::int,$5::int,
       'completed original chat','system',$6::bigint,
       'exchange:' || encode(sha256(convert_to($7,'UTF8')),'hex'),'')
ON CONFLICT DO NOTHING RETURNING id
)sql",
                                      std::array{scope, viewer, day, std::to_string(computed),
                                                 std::to_string(next - current), now, key});
        if (!added.empty())
        {
            db.execute("UPDATE viewer_companionship SET affinity_milli=$3::int "
                       "WHERE scope_id=$1 AND viewer_id=$2::uuid",
                       std::array{scope, viewer, std::to_string(next)});
        }
    }

    void record_completion(Connection& db, const std::string& scope,
                           const meowlive::application::ReplyCompletion& reply)
    {
        const auto rows = db.execute(R"sql(
SELECT viewer_id,source,event_id,day,chat_body FROM companionship_reply_events
WHERE scope_id=$1 AND speech_id=$2 ORDER BY viewer_id,source,event_id
)sql",
                                     std::array{scope, reply.speech_id});
        const auto now = std::to_string(reply.completed_at_ms);
        std::set<std::string> rewarded_viewers;
        for (const auto& row : rows)
        {
            const auto& viewer = row[0].value();
            const auto& source = row[1].value();
            const auto& id = row[2].value();
            const auto& day = row[3].value();
            const auto& body = row[4];
            const auto added = db.execute(R"sql(
INSERT INTO companionship_completed_events(scope_id,source,event_id,viewer_id)
VALUES($1,$2,$3,$4::uuid) ON CONFLICT DO NOTHING RETURNING event_id
)sql",
                                          std::array{scope, source, id, viewer});
            if (added.empty() || !body || body->empty())
            {
                continue;
            }
            const auto unique = db.execute(R"sql(
INSERT INTO companionship_daily_chats(scope_id,viewer_id,day,body_hash)
VALUES($1,$2::uuid,$3::bigint,encode(sha256(convert_to($4,'UTF8')),'hex'))
ON CONFLICT DO NOTHING RETURNING body_hash
)sql",
                                           std::array{scope, viewer, day, *body});
            if (!unique.empty())
            {
                award_chat(db, scope, viewer, day,
                           meowlive::protocol::json::serialize(event_pair(source, id)), now,
                           rewarded_viewers.insert(viewer).second);
            }
        }
        db.execute("UPDATE companionship_replies SET completed_at_ms=$3::bigint "
                   "WHERE scope_id=$1 AND speech_id=$2",
                   std::array{scope, reply.speech_id, now});
    }
}

export namespace meowlive::adapters::storage
{
    class ReplyLedger
    {
    public:
        ReplyLedger(std::string url, std::filesystem::path libpq, std::filesystem::path migrations,
                    std::string scope, int calendar_offset_minutes)
            : url_(std::move(url)), libpq_(std::move(libpq)), migrations_(std::move(migrations)),
              scope_(std::move(scope)), calendar_offset_(std::to_string(calendar_offset_minutes))
        {
            if (calendar_offset_minutes < -840 || calendar_offset_minutes > 840)
            {
                throw std::invalid_argument("viewer_calendar_offset_invalid");
            }
        }

        void complete(const meowlive::application::ReplyCompletion& reply)
        {
            std::set<std::pair<std::string, std::string>> keys;
            for (const auto& event : reply.events)
            {
                if (event.source != "simulator")
                {
                    keys.emplace(event.source, event.id);
                }
            }
            if (keys.empty())
            {
                return;
            }
            meowlive::adapters::storage::PostgresConnection db(url_, libpq_);
            if (!prepared_)
            {
                meowlive::adapters::storage::migrate_postgres(db, migrations_);
                prepared_ = true;
            }
            db.execute("BEGIN");
            db.execute("SET LOCAL statement_timeout='5s'");
            db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                       std::array{scope_});
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope_});
            const auto registered = register_reply(db, scope_, reply, keys);
            if (registered.inserted)
            {
                register_events(db, scope_, reply.speech_id, keys, calendar_offset_);
            }
            if (!registered.completed)
            {
                record_completion(db, scope_, reply);
            }
            db.execute("COMMIT");
        }

    private:
        const std::string url_;
        const std::filesystem::path libpq_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        const std::string calendar_offset_;
        bool prepared_{};
    };
}
