#include <cstdio>
#include <cstdlib>
#include <json/json.h>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.adapters.storage.reply_ledger;
import meowlive.adapters.storage.reply_outbox;
import meowlive.adapters.storage.repositories;
import meowlive.adapters.storage.viewer_queries;
import meowlive.adapters.storage.viewer_affinity;
import meowlive.adapters.storage.viewer_merge;
import meowlive.application.live_session;
import meowlive.application.reply_completion;
import meowlive.domain.live_event;

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    using Connection = meowlive::adapters::storage::PostgresConnection;

    void verify_failure_types(Connection& db)
    {
        try
        {
            db.execute("SELECT 1 / 0");
            throw std::runtime_error("PostgreSQL accepted division by zero");
        }
        catch (const meowlive::adapters::storage::PostgresUnavailable&)
        {
            throw std::runtime_error("PostgreSQL query error was classified as unavailable");
        }
        catch (const meowlive::adapters::storage::PostgresError& error)
        {
            require(std::string_view(error.what()) == "postgres_query_failed_22012",
                    "PostgreSQL query diagnostic lost its SQLSTATE");
        }

        db.execute("SET statement_timeout = '10ms'");
        try
        {
            db.execute("SELECT pg_sleep(0.1)");
            throw std::runtime_error("PostgreSQL did not apply the statement timeout");
        }
        catch (const meowlive::adapters::storage::PostgresUnavailable& error)
        {
            require(std::string_view(error.what()) == "postgres_query_failed_57014",
                    "PostgreSQL timeout did not preserve its SQLSTATE");
        }
        db.execute("RESET statement_timeout");
    }

    template <typename Probe> void wait_for(Probe probe, std::string_view error)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!probe())
        {
            require(std::chrono::steady_clock::now() < deadline, error);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    struct TemporaryOutbox
    {
        explicit TemporaryOutbox(Connection& db)
            : directory(
                  std::filesystem::temp_directory_path() /
                  ("meowlive-rooms-" + db.execute("SELECT gen_random_uuid()").at(0).at(0).value()))
        {
            std::filesystem::create_directory(directory);
        }

        ~TemporaryOutbox()
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        const std::filesystem::path directory;
    };

    void verify_settlement(Connection& db, const std::string& url,
                           const std::filesystem::path& directory,
                           const std::filesystem::path& library,
                           meowlive::adapters::storage::ViewerEvents& writer,
                           const meowlive::domain::LiveEvent& chat, const std::string& viewer,
                           std::uint64_t now)
    {
        constexpr auto scope = "viewer-test";
        auto alternate = chat;
        alternate.id = "fixture-chat-secondary";
        alternate.kind = meowlive::domain::Chat{"other test"};
        auto duplicate_body = chat;
        duplicate_body.id = "fixture-chat-same-body";
        duplicate_body.kind = meowlive::domain::Chat{" test\u3000message\t"};
        using Persistence = meowlive::application::LivePersistence;
        require(writer.append(1, alternate, now + 4) == Persistence::Stored &&
                    writer.append(1, duplicate_body, now + 5) == Persistence::Stored,
                "Reply fixture chats could not be persisted");

        const meowlive::application::ReplyCompletion completion{
            "test-speech",
            now + 6,
            now + 1000,
            {{"bilibili", chat.id}, {"bilibili", alternate.id}, {"bilibili", "fixture-gift"}}};
        const TemporaryOutbox outbox(db);
        const auto primary = outbox.directory / "primary";
        const auto recovery = outbox.directory / "recovery";
        std::atomic<bool> attempted{};
        std::binary_semaphore write_failed(0);
        std::atomic<bool> reported_failure{};
        {
            std::ofstream blocked(primary);
            require(static_cast<bool>(blocked), "Could not block the first outbox write");
        }
        {
            meowlive::adapters::storage::ReplyOutbox offline(
                primary, recovery, scope, "offline",
                [&](const meowlive::application::ReplyCompletion&)
                {
                    attempted = true;
                    throw meowlive::adapters::storage::PostgresUnavailable(
                        "fixture database unavailable");
                },
                [&](std::string_view code)
                {
                    if (code == "reply_completion_outbox_write_failed" &&
                        !reported_failure.exchange(true))
                    {
                        write_failed.release();
                    }
                });
            auto reserved = offline.reserve(completion);
            require(reserved.has_value(), "Outbox could not reserve a reply");
            (*reserved)->confirm(completion.completed_at_ms);
            reserved->reset();
            require(write_failed.try_acquire_for(std::chrono::seconds(5)),
                    "Initial outbox write failure was not observed");
            require(!attempted.load(), "An undurable reply reached the ledger");

            std::vector<std::unique_ptr<meowlive::application::ReplyReservation>> reservations;
            for (unsigned index = 1; index < 128; ++index)
            {
                auto next = offline.reserve(completion);
                require(next.has_value(), "Outbox lost usable reservation capacity");
                reservations.push_back(std::move(*next));
            }
            const auto full = offline.reserve(completion);
            require(!full && full.error() == meowlive::application::ReplyReservationError::Full,
                    "A pending first write did not count toward outbox capacity");
            reservations.pop_back();
            require(offline.reserve(completion).has_value(),
                    "An abandoned reservation did not release outbox capacity");

            require(std::filesystem::remove(primary), "Could not restore outbox writes");
            wait_for(
                [&]
                {
                    return attempted.load();
                },
                "Completion with an initially failed write did not reach replay");
        }

        {
            const auto blocked = outbox.directory / "blocked";
            {
                std::ofstream file(blocked);
                require(static_cast<bool>(file), "Could not block the primary spool at shutdown");
            }
            meowlive::adapters::storage::ReplyOutbox offline(
                blocked, recovery, scope, "shutdown",
                [](const meowlive::application::ReplyCompletion&)
                {
                    throw meowlive::adapters::storage::PostgresUnavailable(
                        "fixture database unavailable during shutdown");
                });
            auto reserved = offline.reserve(completion);
            require(reserved.has_value(), "Could not reserve the shutdown fixture reply");
            (*reserved)->confirm(completion.completed_at_ms);
            require(offline.close() &&
                        std::filesystem::is_regular_file(recovery / "shutdown-1.json"),
                    "Shutdown lost a confirmed reply when the primary spool was blocked");
        }

        meowlive::adapters::storage::ReplyLedger ledger(url, library, directory, scope, 480);
        {
            meowlive::adapters::storage::ReplyOutbox restarted(
                primary, recovery, scope, "restarted",
                [&](const meowlive::application::ReplyCompletion& replayed)
                {
                    ledger.complete(replayed);
                });
            wait_for(
                [&]
                {
                    return std::filesystem::is_empty(primary) &&
                           std::filesystem::is_empty(recovery);
                },
                "Restart did not settle and acknowledge pending completion");
        }
        ledger.complete(completion);
        auto repeated_body = completion;
        repeated_body.speech_id = "test-repeat-body";
        repeated_body.events = {{"bilibili", duplicate_body.id}, {"bilibili", chat.id}};
        ledger.complete(repeated_body);

        const auto affinity = db.execute("SELECT affinity_milli FROM viewer_companionship WHERE "
                                         "scope_id=$1 AND viewer_id=$2::uuid",
                                         std::array{std::string(scope), viewer});
        require(affinity.at(0).at(0) == "200", "One completed reply rewarded a viewer twice");
        const auto awards = db.execute(R"sql(
SELECT computed_delta_milli, applied_delta_milli, day
FROM affinity_ledger WHERE scope_id=$1 AND viewer_id=$2::uuid AND kind='exchange'
ORDER BY computed_delta_milli DESC
)sql",
                                       std::array{std::string(scope), viewer});
        require(awards.size() == 2 && awards[0][0] == "200" && awards[0][1] == "200" &&
                    awards[1][0] == "0" && awards[1][1] == "0" &&
                    awards[0][2] == std::to_string((now + 480u * 60'000u) / 86'400'000u),
                "Computed exchange amounts, body deduplication or local day differ from Rust");
        const auto settled = db.execute(R"sql(
SELECT (SELECT COUNT(*) FROM companionship_replies WHERE scope_id=$1 AND completed_at_ms IS NOT NULL),
       (SELECT COUNT(*) FROM companionship_completed_events WHERE scope_id=$1)
)sql",
                                        std::array{std::string(scope)});
        require(settled.at(0).at(0) == "2" && settled.at(0).at(1) == "4",
                "Scope and event idempotence did not survive reply retry");
    }

    void verify_memories(Connection& db, const std::string& viewer_id,
                         meowlive::adapters::storage::ViewerQueries& reader,
                         const meowlive::domain::LiveEvent& chat, std::uint64_t now)
    {
        constexpr auto sql = R"sql(
INSERT INTO memories(id, scope_id, viewer_id, key, value, kind, status,
                     deleted, created_at_ms, updated_at_ms, expires_at_ms)
VALUES (gen_random_uuid(), 'viewer-test', $1::uuid, $2, 'a memory', 'preference',
        $3, $4::bool, $5::bigint, $5::bigint, NULLIF($6, '')::bigint)
)sql";
        const auto now_text = std::to_string(now);
        const auto seed =
            [&](std::string key, std::string status, std::string deleted, std::string expires)
        {
            db.execute(sql, std::array{viewer_id, key, status, deleted, now_text, expires});
        };
        seed("valid", "long_term", "false", "");
        seed("expired", "short_term", "false", now_text);
        seed("unconfirmed", "candidate", "false", "");
        seed("deleted", "long_term", "true", "");

        const auto memory = reader.memories(chat, now, std::chrono::seconds(1));
        require(memory.size() == 1 && memory[0u]["key"] == "valid",
                "Expired, unconfirmed or deleted memories reached the model query");
        auto wrong_identity = chat;
        wrong_identity.viewer_identity->external_id = "another-viewer";
        require(reader.memories(wrong_identity, now, std::chrono::seconds(1)).empty(),
                "The event ID alone exposed another viewer's memories");
    }

    void require_failure(Connection& db, const std::filesystem::path& migrations,
                         std::string_view expected)
    {
        try
        {
            meowlive::adapters::storage::migrate_postgres(db, migrations);
        }
        catch (const std::runtime_error& error)
        {
            require(error.what() == expected, error.what());
            return;
        }
        throw std::runtime_error("Incompatible migration state was accepted");
    }

    void verify_merge(Connection& db, const std::string& url,
                      const std::filesystem::path& directory, const std::filesystem::path& library,
                      meowlive::adapters::storage::ViewerEvents& writer,
                      const meowlive::domain::LiveEvent& example, const std::string& target,
                      std::uint64_t now)
    {
        const std::string scope = "viewer-test";
        auto other = example;
        other.id = "fixture-source";
        other.viewer_identity->external_id = "other-account";
        require(writer.append(2, other, now + 5000) ==
                    meowlive::application::LivePersistence::Stored,
                "Source viewer could not be created");
        const auto source = db.execute("SELECT viewer_id FROM viewer_events WHERE "
                                       "scope_id=$1 AND event_id=$2",
                                       std::array{scope, other.id})
                                .at(0)
                                .at(0)
                                .value();
        const std::array viewers{scope, source, target};
        db.execute(R"sql(
INSERT INTO memories(id,scope_id,viewer_id,key,value,kind,status,created_at_ms,updated_at_ms)
VALUES(gen_random_uuid(),$1,$2::uuid,'valid','other value','preference','long_term',1,1)
)sql",
                   std::array{scope, source});
        db.execute(R"sql(
INSERT INTO relationship_facts(scope_id,id,source_kind,source_id,target_kind,target_id,kind,
                               confirmation,evidence,created_at_ms,updated_at_ms)
VALUES($1,gen_random_uuid(),'viewer',$2,'viewer',$3,'friend','confirmed','[]'::jsonb,1,1)
)sql",
                   viewers);
        db.execute(R"sql(
INSERT INTO viewer_presence(scope_id,viewer_id,day) VALUES
($1,$2::uuid,1),($1,$3::uuid,1),($1,$3::uuid,2)
)sql",
                   viewers);
        meowlive::adapters::storage::ViewerMerge merge(url, library, directory, scope);
        const auto preview = merge.preview(source, target);
        require(preview && (*preview)["resulting_affinity_milli"] == 200 &&
                    (*preview)["resulting_familiarity_milli"] == 2000,
                "Merge preview disagreed with score union");
        meowlive::adapters::storage::ViewerMergeRequest request{
            source,
            target,
            (*preview)["revision"].asInt64(),
            (*preview)["fingerprint"].asString(),
            "merge-fixture",
            "Verify explicit identity ownership"};
        db.execute("UPDATE viewers SET current_alias='changed' WHERE scope_id=$1 AND id=$2::uuid",
                   std::array{scope, source});
        require(!merge.apply(request, now + 5010), "A stale merge preview allowed mutation");
        const auto refreshed = merge.preview(source, target);
        require(refreshed.has_value(), "Rejected merge lost viewer state");
        request.revision = (*refreshed)["revision"].asInt64();
        request.fingerprint = (*refreshed)["fingerprint"].asString();
        const auto applied = merge.apply(request, now + 5010);
        require(applied && (*applied)["canonical_viewer_id"] == target &&
                    merge.apply(request, now + 5011) == applied && !merge.preview(source, target),
                "Confirmed merge, replay or source tombstone mismatch");
        const auto data = db.execute(R"sql(
SELECT (SELECT count(*) FROM viewer_identities WHERE scope_id=$1 AND viewer_id=$3::uuid),
       (SELECT count(*) FROM memories WHERE scope_id=$1 AND viewer_id=$3::uuid
          AND key='valid' AND locked AND status='candidate'),
       (SELECT count(*) FROM relationship_facts f JOIN relationship_outbox o
          ON o.scope_id=f.scope_id AND o.fact_id=f.id AND o.version=f.version
          WHERE f.scope_id=$1 AND f.source_id=$3::uuid::text AND f.target_id=$3::uuid::text AND f.deleted
            AND f.confirmation='claimed' AND o.state='pending'),
       (SELECT count(*) FROM affinity_ledger WHERE scope_id=$1 AND viewer_id=$3::uuid
          AND original_viewer_id IS NOT NULL)
WHERE EXISTS (SELECT FROM viewers WHERE scope_id=$1 AND id=$2::uuid AND merged_into=$3::uuid)
)sql",
                                     viewers)
                              .at(0);
        require(data.at(0) == "2" && data.at(1) == "2" && data.at(2) == "1" && data.at(3) == "6",
                "Identity merge lost frozen memories, relationship refresh or ledger audit");
        other.id = "fixture-source-after-merge";
        require(
            writer.append(2, other, now + 5020) == meowlive::application::LivePersistence::Stored &&
                db.execute("SELECT viewer_id FROM viewer_events WHERE scope_id=$1 AND event_id=$2",
                           std::array{scope, other.id})
                        .at(0)
                        .at(0) == target,
            "Source platform identity did not resolve to the canonical viewer");
    }

    void verify_viewers(const std::string& url, const std::filesystem::path& directory,
                        const std::filesystem::path& library)
    {
        constexpr auto scope = "viewer-test";
        constexpr std::uint64_t now = 1'800'000'000'000;
        const meowlive::domain::ViewerIdentity identity{
            "app:test", meowlive::domain::ViewerIdentityKind::OpenId, "fixture-account"};
        meowlive::adapters::storage::ViewerEvents writer(url, library, directory, scope,
                                                         "test-run");
        using Result = meowlive::application::LivePersistence;
        const meowlive::domain::LiveEvent chat{.id = "fixture-chat",
                                               .source = "bilibili",
                                               .viewer = "Current alias",
                                               .viewer_identity = identity,
                                               .occurred_at_ms = now,
                                               .kind = meowlive::domain::Chat{"test message"}};
        const meowlive::domain::LiveEvent gift{
            .id = "fixture-gift",
            .source = "bilibili",
            .viewer = "Old alias",
            .viewer_identity = identity,
            .occurred_at_ms = now - 100,
            .gift_metadata = meowlive::domain::GiftMetadata{.price = 1000, .paid = true},
            .kind = meowlive::domain::Gift{"Test gift", 2}};
        const meowlive::domain::LiveEvent anonymous{.id = "fixture-enter",
                                                    .source = "bilibili",
                                                    .viewer = "Anonymous",
                                                    .occurred_at_ms = now,
                                                    .kind = meowlive::domain::RoomEnter{}};
        require(writer.append(1, chat, now) == Result::Stored &&
                    writer.append(1, chat, now + 1) == Result::Duplicate &&
                    writer.append(1, gift, now + 2) == Result::Stored &&
                    writer.append(1, anonymous, now + 3) == Result::Stored,
                "Event ingestion or replay deduplication failed");

        meowlive::adapters::storage::ViewerQueries reader(url, library, directory, scope);
        require(reader.available(), "Viewer repository schema is unavailable");
        const auto profiles = reader.viewers(50, 0);
        const auto& viewer = profiles[0u];
        require(profiles.size() == 1 && viewer["current_alias"] == "Current alias" &&
                    viewer["alias_observed_at_ms"].asUInt64() == now &&
                    viewer["identities"][0u]["external_id"] == "fixture-account" &&
                    viewer["aliases"].size() == 2,
                "Viewer identity, alias order or current alias did not survive the ingest");
        const auto id = viewer["viewer_id"].asString();
        require(reader.viewer(id) == viewer && reader.viewers(50, 1).empty(),
                "Viewer detail or bounded pagination failed");
        const auto events = reader.events(3, 0);
        require(events.size() == 3 && events[0u]["kind"]["type"] == "room_enter" &&
                    events[0u]["viewer_id"].isNull() && events[1u]["kind"]["type"] == "gift" &&
                    events[1u]["gift_metadata"]["paid"] == true &&
                    events[1u]["gift_metadata"]["price"] == 1000 &&
                    events[2u]["kind"]["text"] == "test message" && events[2u]["viewer_id"] == id &&
                    reader.events(1, 1)[0u] == events[1u],
                "Event kind, optional identity, metadata or pagination was lost");
        meowlive::adapters::storage::ViewerQueries another_scope(url, library, directory,
                                                                 "isolated-test");
        require(another_scope.viewers(50, 0).empty() && another_scope.events(50, 0).empty() &&
                    !another_scope.viewer(id),
                "Viewer data crossed observation scopes");
        Connection db(url, library);
        verify_memories(db, id, reader, chat, now);
        verify_settlement(db, url, directory, library, writer, chat, id, now);
        meowlive::adapters::storage::ViewerAffinity affinity(url, library, directory, scope, 480);
        const auto adjust =
            affinity.change(id, "adjust-fixture", "Correction", 100'000, "", now + 2000);
        require(adjust && affinity.details(id)->operator[]("affinity_milli") == 100'000 &&
                    affinity.change(id, "adjust-fixture", "Correction", 100'000, "", now + 2001) ==
                        adjust &&
                    !affinity.change(id, "adjust-fixture", "Changed", 100'000, "", now + 2001),
                "Affinity clamp or adjustment retry violated ledger invariants");
        const auto undo = affinity.change(id, "reverse-fixture", "Undo", 0, *adjust, now + 3000);
        const auto restored = affinity.details(id);
        require(undo && restored && (*restored)["affinity_milli"] == 200 &&
                    affinity.change(id, "reverse-fixture", "Undo", 0, *adjust, now + 3001) ==
                        undo &&
                    !affinity.change(id, "reverse-again", "Undo", 0, *adjust, now + 3001),
                "Reversal did not undo the applied amount exactly once");
        const auto& ledger = (*restored)["ledger"];
        require(ledger[0u]["kind"] == "reversal" && ledger[0u]["computed_delta_milli"] == -99'800 &&
                    ledger[1u]["ledger_id"] == *adjust && ledger[1u]["reversible"] == false,
                "Read-only companionship view lost reversal history");
        verify_merge(db, url, directory, library, writer, chat, id, now);
    }

    void verify(const std::filesystem::path& directory, const std::filesystem::path& library)
    {
        const auto* url = std::getenv("MEOWLIVE_TEST_POSTGRES_URL");
        require(url && *url, "Set MEOWLIVE_TEST_POSTGRES_URL to an isolated test database");
        Connection db(url, library);
        const auto name = db.execute("SELECT current_database()").at(0).at(0).value();
        require(name == "meowlive_test_fresh" || name == "meowlive_test_legacy",
                "Use meowlive_test_fresh or meowlive_test_legacy as isolated database name");
        verify_failure_types(db);

        const auto metadata = db.execute("SELECT to_regclass('_sqlx_migrations') IS NOT NULL");
        const bool legacy = name == "meowlive_test_legacy";
        require(metadata.at(0).at(0) == (legacy ? "t" : "f"),
                "Expected a blank database or SQLx migration history through version 8");
        meowlive::adapters::storage::PostgresRows previous;
        if (legacy)
        {
            previous = db.execute("SELECT version, installed_on, encode(checksum, 'hex') "
                                  "FROM _sqlx_migrations ORDER BY version");
            require(previous.size() == 8, "Expected an old migration history through version 8");
        }

        meowlive::adapters::storage::migrate_postgres(db, directory);
        const auto installed = db.execute("SELECT version, installed_on, encode(checksum, 'hex') "
                                          "FROM _sqlx_migrations ORDER BY version");
        require(installed.size() == 9, "Expected the full migration history");
        for (std::size_t index = 0; index < previous.size(); ++index)
        {
            require(installed[index] == previous[index], "A SQLx migration row was rewritten");
        }
        meowlive::adapters::storage::migrate_postgres(db, directory);
        require(db.execute("SELECT version, installed_on, encode(checksum, 'hex') "
                           "FROM _sqlx_migrations ORDER BY version") == installed,
                "Migration retry did not preserve installed rows");
        const auto schema = db.execute("SELECT vector_dims('[1,2,3]'::vector), "
                                       "pg_get_constraintdef(oid) "
                                       "FROM pg_constraint WHERE conname = "
                                       "'viewer_events_event_type_check' "
                                       "AND conrelid = 'viewer_events'::regclass");
        require(schema.at(0).at(0) == "3" && schema.at(0).at(1)->contains("super_chat") &&
                    schema.at(0).at(1)->contains("room_enter"),
                "Vector or event constraint is missing after migration");

        db.execute("UPDATE _sqlx_migrations SET success = false WHERE version = 8");
        require_failure(db, directory, "postgres_migration_dirty_8");
        db.execute("UPDATE _sqlx_migrations SET success = true WHERE version = 8");
        db.execute("UPDATE _sqlx_migrations SET checksum = decode('ff','hex') WHERE version = 8");
        require_failure(db, directory, "postgres_migration_checksum_8");
        const std::array parameters{installed[7][2].value()};
        db.execute("UPDATE _sqlx_migrations SET checksum = decode($1,'hex') WHERE version = 8",
                   parameters);
        db.execute("UPDATE _sqlx_migrations SET version = 10 WHERE version = 9");
        require_failure(db, directory, "postgres_migration_version_unsupported");
        db.execute("UPDATE _sqlx_migrations SET version = 9 WHERE version = 10");
        meowlive::adapters::storage::migrate_postgres(db, directory);
        verify_viewers(url, directory, library);
        std::println("PostgreSQL SQLx migration verification passed ({})",
                     legacy ? "legacy" : "fresh");
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        require(argc >= 2 && argc <= 3, "meowlive-postgres-integration MIGRATIONS [LIBPQ]");
        verify(std::filesystem::path(argv[1]),
               argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "PostgreSQL integration failed: {}", error.what());
        return 1;
    }
}
