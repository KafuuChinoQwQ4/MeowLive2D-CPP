module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.memory_vectors;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;

export namespace meowlive::adapters::storage
{
    struct MemoryVectorJob
    {
        std::string memory_id;
        std::string version;
        std::string value;
        std::string token;
    };

    class MemoryVectors
    {
    public:
        MemoryVectors(std::string url, std::filesystem::path library,
                      std::filesystem::path migrations, std::string scope, std::string model,
                      unsigned dimensions)
            : url_(std::move(url)), library_(std::move(library)),
              migrations_(std::move(migrations)), scope_(std::move(scope)),
              model_(std::move(model)), dimensions_(std::to_string(dimensions))
        {
        }

        std::optional<meowlive::adapters::storage::MemoryVectorJob> claim(std::int64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            begin(db);
            const auto timestamp = std::to_string(now);
            const auto rows = db.execute(R"sql(
WITH eligible AS (
    SELECT m.id,m.version,m.value FROM memories m
    LEFT JOIN memory_vectors v ON (v.scope_id,v.memory_id,v.version)=(m.scope_id,m.id,m.version)
        AND v.model=$2 AND v.dimensions=$3::int
    LEFT JOIN memory_embedding_jobs j ON (j.scope_id,j.memory_id)=(m.scope_id,m.id)
        AND j.model=$2 AND j.dimensions=$3::int
    WHERE m.scope_id=$1 AND NOT m.deleted AND m.status IN('short_term','long_term')
        AND (m.expires_at_ms IS NULL OR m.expires_at_ms>$4::bigint) AND v.memory_id IS NULL
        AND (j.memory_id IS NULL OR j.version<>m.version OR (j.lease_until_ms<=$4::bigint AND j.attempts<3))
    ORDER BY m.updated_at_ms,m.id LIMIT 1
), leased AS (
    INSERT INTO memory_embedding_jobs(scope_id,memory_id,version,model,dimensions,token,lease_until_ms)
    SELECT $1,id,version,$2,$3::int,gen_random_uuid(),$4::bigint+65000 FROM eligible
    ON CONFLICT(scope_id,memory_id,model,dimensions) DO UPDATE SET version=excluded.version,
        token=excluded.token,lease_until_ms=excluded.lease_until_ms,
        attempts=CASE WHEN memory_embedding_jobs.version<>excluded.version THEN 1
            ELSE memory_embedding_jobs.attempts+1 END
    RETURNING memory_id,token
)
SELECT eligible.id,eligible.version,eligible.value,leased.token FROM eligible
JOIN leased ON leased.memory_id=eligible.id
)sql",
                                         std::array{scope_, model_, dimensions_, timestamp});
            db.execute("COMMIT");
            if (rows.empty())
            {
                return {};
            }
            const auto& row = rows[0];
            return meowlive::adapters::storage::MemoryVectorJob{row[0].value(), row[1].value(),
                                                                row[2].value(), row[3].value()};
        }

        void store(const meowlive::adapters::storage::MemoryVectorJob& job,
                   const std::string& embedding, std::int64_t now)
        {
            meowlive::adapters::storage::PostgresConnection db(url_, library_);
            begin(db);
            db.execute(R"sql(
WITH finished AS (
    DELETE FROM memory_embedding_jobs j USING memories m
    WHERE j.scope_id=$1 AND j.memory_id=$2::uuid AND j.version=$3::bigint AND j.model=$4
        AND j.dimensions=$5::int AND j.token=$6::uuid AND j.lease_until_ms>$7::bigint
        AND (m.scope_id,m.id,m.version)=(j.scope_id,j.memory_id,j.version)
        AND NOT m.deleted AND m.status IN('short_term','long_term')
        AND (m.expires_at_ms IS NULL OR m.expires_at_ms>$7::bigint)
    RETURNING j.*
)
INSERT INTO memory_vectors(scope_id,memory_id,version,model,dimensions,embedding)
SELECT scope_id,memory_id,version,model,dimensions,$8::vector FROM finished
ON CONFLICT(scope_id,memory_id,model,dimensions) DO UPDATE SET
    version=excluded.version,embedding=excluded.embedding
)sql",
                       std::array{scope_, job.memory_id, job.version, model_, dimensions_,
                                  job.token, std::to_string(now), embedding});
            db.execute("COMMIT");
        }

    private:
        void begin(meowlive::adapters::storage::PostgresConnection& db)
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
            db.execute("SELECT revision FROM memory_scopes WHERE scope_id=$1 FOR UPDATE",
                       std::array{scope_});
        }

        const std::string url_;
        const std::filesystem::path library_;
        const std::filesystem::path migrations_;
        const std::string scope_;
        const std::string model_;
        const std::string dimensions_;
        std::once_flag initialized_;
    };
}
