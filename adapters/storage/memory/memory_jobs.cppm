module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.memory_jobs;

import meowlive.standard;

import meowlive.adapters.storage.memory_records;
import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.application.memory_extraction;

export namespace meowlive::adapters::storage
{
    struct MemoryJob
    {
        std::string id;
        std::string token;
        std::string revision;
        meowlive::application::MemorySource source;
    };

    class MemoryJobs
    {
    public:
        MemoryJobs(std::string url, std::filesystem::path library, std::filesystem::path migrations,
                   std::string scope)
            : url_(std::move(url)), library_(std::move(library)),
              migrations_(std::move(migrations)), scope_(std::move(scope))
        {
        }

        std::optional<meowlive::adapters::storage::MemoryJob> claim(std::int64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            const auto revision = begin(db);
            const auto timestamp = std::to_string(now);
            db.execute(R"sql(
UPDATE memory_jobs SET state='failed',token=NULL WHERE scope_id=$1 AND state='running'
    AND lease_until_ms<=$2::bigint AND attempts>=3
)sql",
                       std::array{scope_, timestamp});
            const auto rows = db.execute(R"sql(
UPDATE memory_jobs SET state='running',attempts=attempts+1,token=gen_random_uuid(),
    lease_until_ms=$2::bigint+65000,revision=$3::bigint
WHERE scope_id=$1 AND id=(
    SELECT id FROM memory_jobs WHERE scope_id=$1 AND attempts<3 AND body<>''
        AND occurred_at_ms BETWEEN $2::bigint-2592000000 AND $2::bigint
        AND ((state='pending' AND available_at_ms<=$2::bigint)
             OR (state='running' AND lease_until_ms<=$2::bigint))
    ORDER BY available_at_ms,id LIMIT 1
) AND NOT EXISTS (SELECT FROM memory_jobs WHERE scope_id=$1 AND state='running'
    AND lease_until_ms>$2::bigint)
RETURNING id,token,source,viewer_id,event_id,body,occurred_at_ms,day
)sql",
                                         std::array{scope_, timestamp, revision});
            db.execute("COMMIT");
            if (rows.empty())
            {
                return {};
            }
            const auto& row = rows[0];
            return meowlive::adapters::storage::MemoryJob{
                row[0].value(),
                row[1].value(),
                revision,
                {row[2].value(), row[3].value(), row[4].value(), row[5].value(),
                 std::stoll(row[6].value()), std::stoll(row[7].value())}};
        }

        void finish(const meowlive::adapters::storage::MemoryJob& job,
                    std::span<const meowlive::application::MemoryCandidate> candidates,
                    std::int64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            const auto revision = begin(db);
            const auto timestamp = std::to_string(now);
            const auto rows =
                db.execute(R"sql(
UPDATE memory_jobs SET state='done',token=NULL,lease_until_ms=NULL,body=''
WHERE scope_id=$1 AND id=$2::uuid AND token=$3::uuid AND state='running'
    AND lease_until_ms>$4::bigint AND revision=$5::bigint
RETURNING id
)sql",
                           std::array{scope_, job.id, job.token, timestamp, job.revision});
            if (!rows.empty() && revision == job.revision && !candidates.empty())
            {
                for (const auto& candidate : candidates)
                {
                    meowlive::adapters::storage::merge_extracted_memory(db, scope_, job.source,
                                                                        candidate, now);
                }
                meowlive::adapters::storage::prune_extracted_memories(
                    db, scope_, job.source.viewer_id, timestamp);
            }
            db.execute("COMMIT");
        }

        void fail(const meowlive::adapters::storage::MemoryJob& job, std::int64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            begin(db);
            db.execute(R"sql(
UPDATE memory_jobs SET state=CASE WHEN attempts>=3 THEN 'failed' ELSE 'pending' END,
    token=NULL,lease_until_ms=NULL,available_at_ms=$4::bigint+(attempts*attempts*1000)
WHERE scope_id=$1 AND id=$2::uuid AND token=$3::uuid AND state='running'
    AND lease_until_ms>$4::bigint
)sql",
                       std::array{scope_, job.id, job.token, std::to_string(now)});
            db.execute("COMMIT");
        }

        void expire(std::int64_t now, bool purge_source_text)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            begin(db);
            const auto timestamp = std::to_string(now);
            db.execute(R"sql(
UPDATE memories SET status='expired',version=version+1
WHERE scope_id=$1 AND status<>'expired' AND expires_at_ms<=$2::bigint
)sql",
                       std::array{scope_, timestamp});
            if (purge_source_text)
            {
                db.execute(R"sql(
UPDATE viewer_events SET payload=jsonb_set(payload,'{text}','""'::jsonb)
WHERE scope_id=$1 AND event_type='chat' AND occurred_at_ms<$2::bigint-2592000000
    AND payload->>'text'<>''
)sql",
                           std::array{scope_, timestamp});
                db.execute(R"sql(
UPDATE companionship_reply_events e SET chat_body=NULL
FROM companionship_replies r
WHERE e.scope_id=$1 AND e.scope_id=r.scope_id AND e.speech_id=r.speech_id
    AND r.generated_at_ms<$2::bigint-2592000000 AND e.chat_body IS NOT NULL
)sql",
                           std::array{scope_, timestamp});
            }
            db.execute(R"sql(
UPDATE memory_jobs SET body='',token=NULL,lease_until_ms=NULL,
    state=CASE WHEN state IN('pending','running') THEN 'failed' ELSE state END
WHERE scope_id=$1 AND occurred_at_ms<$2::bigint-2592000000 AND body<>''
)sql",
                       std::array{scope_, timestamp});
            db.execute(R"sql(
DELETE FROM memory_vectors v USING memories m WHERE v.scope_id=$1 AND m.scope_id=v.scope_id
    AND m.id=v.memory_id AND (m.deleted OR m.status='expired' OR m.version<>v.version)
)sql",
                       std::array{scope_});
            db.execute("COMMIT");
        }

    private:
        std::string begin(meowlive::adapters::storage::PostgresConnection& db)
        {
            std::call_once(initialized_,
                           [&]
                           {
                               meowlive::adapters::storage::migrate_postgres(db, migrations_);
                           });
            db.execute("BEGIN");
            db.execute("SET LOCAL statement_timeout='5s'");
            db.execute("INSERT INTO memory_scopes(scope_id) VALUES($1) ON CONFLICT DO NOTHING",
                       std::array{scope_});
            return db
                .execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                         std::array{scope_})[0][0]
                .value();
        }

        const std::string url_;
        const std::filesystem::path library_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        std::once_flag initialized_;
    };
}
