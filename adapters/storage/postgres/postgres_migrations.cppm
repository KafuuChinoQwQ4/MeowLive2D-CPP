module;

#include <zlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.postgres_migrations;

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.platform.exception_diagnostics;

namespace
{
    struct Migration
    {
        std::string_view file;
        std::string_view description;
        std::string_view checksum;
    };

    constexpr std::array migrations{
        Migration{"0001_viewer_identity_events.sql", "viewer identity events",
                  "88b5a9321adadba13d86d783710203c8a6897dadc7b0f918d5b84ce5e3296efbd85ed1b9154529e9"
                  "7caeec2a079d6200"},
        Migration{"0002_companionship.sql", "companionship",
                  "f9bd45281662b2fa68100f8cb8657f0dc0b586f6f13415ba8cb4d78b3f6e6b8550464d1f0cdbfeeb"
                  "9b9eb32943797c66"},
        Migration{"0003_memories.sql", "memories",
                  "d7375b20ca5d3b31ccce9df864627c2bf335a66ff76ce18c1027fe637787059dd35cb2c30392a3d0"
                  "d0577a92350591ac"},
        Migration{"0004_relationships.sql", "relationships",
                  "c2280464c64df80c8f52772977d754fa6969dc1d6362a4a8a244941fabd99b47d9ea3f6a298c367c"
                  "c2dc3deb5fd696a8"},
        Migration{"0005_memory_cleanup.sql", "memory cleanup",
                  "2b05ffadabd66de84491078ba612c2df2bebc895c5887f568f1d2f6cc06fb2a4d2c72ab295334bea"
                  "079b1081140c13bb"},
        Migration{"0006_memory_operations.sql", "memory operations",
                  "efa6aadb48933362ad3886dab774b1377352ca5c78cf44facd8c8fcc08ca87ce4d5311687f211b1d"
                  "8a8f0203e6e15ffe"},
        Migration{"0007_viewer_merge.sql", "viewer merge",
                  "c10e85e559f8a440df6a514f3ea4a37b8f5242da1d3c30a13148d93077b02db43573bb5dec17c9a4"
                  "3b0afa6abdfed712"},
        Migration{"0008_memory_graph_invalidation.sql", "memory graph invalidation",
                  "fece8efe705980a3cefb2ad4f3d0766498c4b438705fe0913c5a37d274d49480b7fb9f98153a9453"
                  "1858b4d3a0e94edc"},
        Migration{"0009_superchat_room_enter_events.sql", "superchat room enter events",
                  "f9bd2d08718898399d2d742af694ce66c48c062f28f432b2e1f48169d11343774a082a506532d1cd"
                  "d11fa7c3e7981b5b"},
    };

    std::string sha384(std::string_view payload)
    {
        std::array<unsigned char, 48> digest{};
#if defined(_WIN32)
        auto* input = reinterpret_cast<unsigned char*>(const_cast<char*>(payload.data()));
        if (::BCryptHash(BCRYPT_SHA384_ALG_HANDLE, nullptr, 0, input,
                         static_cast<::ULONG>(payload.size()), digest.data(),
                         static_cast<::ULONG>(digest.size())) < 0)
#else
        unsigned size{};
        if (::EVP_Digest(payload.data(), payload.size(), digest.data(), &size, ::EVP_sha384(),
                         nullptr) != 1 ||
            size != digest.size())
#endif
        {
            throw std::runtime_error("postgres_migration_hash_failed");
        }
        constexpr std::string_view hex{"0123456789abcdef"};
        std::string output;
        output.reserve(digest.size() * 2);
        for (const auto byte : digest)
        {
            output += hex[byte >> 4];
            output += hex[byte & 0x0f];
        }
        return output;
    }

    std::string load_migration(const std::filesystem::path& directory, const Migration& migration)
    {
        constexpr std::size_t limit = 128 * 1024;
        std::ifstream file(directory / migration.file, std::ios::binary);
        std::string script(limit + 1, '\0');
        file.read(script.data(), static_cast<std::streamsize>(script.size()));
        if (!file.eof() || file.bad() || file.gcount() == 0 ||
            file.gcount() > static_cast<std::streamsize>(limit))
        {
            throw std::runtime_error("postgres_migration_file_invalid_" +
                                     std::string(migration.file));
        }
        script.resize(static_cast<std::size_t>(file.gcount()));
        if (sha384(script) != migration.checksum)
        {
            throw std::runtime_error("postgres_migration_file_checksum_" +
                                     std::string(migration.file));
        }
        return script;
    }

    constexpr auto migrations_table = R"sql(
CREATE TABLE IF NOT EXISTS _sqlx_migrations (
    version BIGINT PRIMARY KEY,
    description TEXT NOT NULL,
    installed_on TIMESTAMPTZ NOT NULL DEFAULT now(),
    success BOOLEAN NOT NULL,
    checksum BYTEA NOT NULL,
    execution_time BIGINT NOT NULL
);
)sql";

    using Connection = meowlive::adapters::storage::PostgresConnection;

    std::string database_lock(Connection& db)
    {
        const auto current = db.execute("SELECT current_database()");
        const auto& name = current.at(0).at(0).value();
        const auto crc = ::crc32(0, reinterpret_cast<const unsigned char*>(name.data()),
                                 static_cast<unsigned int>(name.size()));
        // SQLx 0.8 uses the database name's CRC-32 and this multiplier.
        return std::to_string(0x3d32ad9eLL * static_cast<std::int64_t>(crc));
    }

    void apply(Connection& db, const std::string& script, const Migration& migration,
               std::size_t version)
    {
        const auto started = std::chrono::steady_clock::now();
        db.execute(script);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        const std::array arguments{std::to_string(version), std::string(migration.description),
                                   std::string(migration.checksum),
                                   std::to_string(elapsed.count())};
        db.execute("INSERT INTO _sqlx_migrations(version, description, success, checksum, "
                   "execution_time) VALUES ($1::bigint, $2, true, decode($3, 'hex'), $4::bigint)",
                   arguments);
    }

    void verify_schema(Connection& db)
    {
        const auto state = db.execute(R"sql(
SELECT EXISTS (SELECT FROM pg_extension WHERE extname = 'vector'),
       to_regclass('viewers') IS NOT NULL,
       to_regclass('viewer_events_scope_received_idx') IS NOT NULL,
       to_regclass('memory_vectors') IS NOT NULL,
       to_regclass('relationship_outbox_ready') IS NOT NULL,
       EXISTS (SELECT FROM pg_constraint WHERE conrelid = to_regclass('viewer_events')
               AND conname = 'viewer_events_event_type_check' AND convalidated)
)sql")
                               .at(0);
        for (const auto& valid : state)
        {
            if (valid != "t")
            {
                throw std::runtime_error("postgres_migration_schema_incomplete");
            }
        }
    }
}

export namespace meowlive::adapters::storage
{
    void migrate_postgres(meowlive::adapters::storage::PostgresConnection& db,
                          const std::filesystem::path& directory)
    {
        std::array<std::string, migrations.size()> scripts;
        for (std::size_t index = 0; index < migrations.size(); ++index)
        {
            scripts[index] = load_migration(directory, migrations[index]);
        }
        const std::array lock{database_lock(db)};
        db.execute("BEGIN");
        try
        {
            db.execute("SET LOCAL lock_timeout = '30s'");
            db.execute("SELECT pg_advisory_xact_lock($1::bigint)", lock);
            db.execute(migrations_table);
            const auto rows = db.execute("SELECT version, success, encode(checksum, 'hex') "
                                         "FROM _sqlx_migrations ORDER BY version");
            if (rows.size() > migrations.size())
            {
                throw std::runtime_error("postgres_migration_version_unsupported");
            }
            for (std::size_t index = 0; index < rows.size(); ++index)
            {
                const auto& row = rows[index];
                if (row[0] != std::to_string(index + 1))
                {
                    throw std::runtime_error("postgres_migration_version_unsupported");
                }
                if (row[1] != "t")
                {
                    throw std::runtime_error("postgres_migration_dirty_" + *row[0]);
                }
                if (row[2] != migrations[index].checksum)
                {
                    throw std::runtime_error("postgres_migration_checksum_" + *row[0]);
                }
            }
            for (std::size_t index = rows.size(); index < migrations.size(); ++index)
            {
                apply(db, scripts[index], migrations[index], index + 1);
            }
            verify_schema(db);
            db.execute("COMMIT");
        }
        catch (...)
        {
            try
            {
                db.execute("ROLLBACK");
            }
            catch (...)
            {
                meowlive::platform::log_current_exception("postgres.migration.rollback");
            }
            throw;
        }
    }
}
