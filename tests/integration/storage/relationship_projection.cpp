#include <cstdio>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.adapters.storage.neo4j_projection;
import meowlive.adapters.storage.postgres;
import meowlive.adapters.storage.postgres_migrations;
import meowlive.adapters.storage.relationship_admin;
import meowlive.adapters.storage.relationship_derivation;
import meowlive.adapters.storage.relationship_outbox;
import meowlive.adapters.storage.relationship_queries;
import meowlive.server.graph_worker;

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string environment(const char* name)
    {
        const auto value = std::getenv(name);
        if (!value || !*value)
        {
            throw std::runtime_error(std::string("Missing environment variable ") + name);
        }
        return value;
    }

    void verify(const std::filesystem::path& migrations, const std::filesystem::path& library)
    {
        const auto url = environment("MEOWLIVE_TEST_POSTGRES_URL");
        const auto endpoint = environment("MEOWLIVE_TEST_NEO4J_URL");
        const auto user = environment("MEOWLIVE_TEST_NEO4J_USERNAME");
        const auto password = environment("MEOWLIVE_TEST_NEO4J_PASSWORD");
        meowlive::adapters::storage::PostgresConnection db(url, library);
        require(db.execute("SELECT current_database()").at(0).at(0) == "meowlive_test_graph",
                "Use the isolated meowlive_test_graph database");
        meowlive::adapters::storage::migrate_postgres(db, migrations);
        const auto fixture = db.execute("SELECT gen_random_uuid(),gen_random_uuid(),"
                                        "gen_random_uuid(),gen_random_uuid(),gen_random_uuid()")[0];
        const auto scope = "graph-fixture-" + fixture[0].value();
        const auto viewer = fixture[1].value();
        const auto other_viewer = fixture[2].value();
        const auto first = fixture[3].value();
        const auto second = fixture[4].value();
        const auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const auto time = std::to_string(now);
        db.execute("INSERT INTO viewers(scope_id,id,created_at_ms) VALUES "
                   "($1,$2::uuid,$4::bigint),($1,$3::uuid,$4::bigint)",
                   std::array{scope, viewer, other_viewer, time});
        db.execute("INSERT INTO live_sessions(scope_id,session_id,first_received_at_ms) "
                   "VALUES($1,'fixture',$2::bigint)",
                   std::array{scope, time});
        db.execute(R"sql(
INSERT INTO viewer_events(scope_id,source,event_id,session_id,viewer_id,viewer_alias,event_type,
                          payload,occurred_at_ms,received_at_ms)
VALUES($1,'fixture','a','fixture',$2::uuid,'a','chat','{"text":"我喜欢C++"}',$4::bigint,$4::bigint),
      ($1,'fixture','b','fixture',$3::uuid,'b','chat','{"text":"我喜欢C++"}',$4::bigint,$4::bigint)
)sql",
                   std::array{scope, viewer, other_viewer, time});
        db.execute(R"sql(
INSERT INTO memories(id,scope_id,viewer_id,key,value,kind,status,created_at_ms,updated_at_ms)
VALUES($1::uuid,$5,$2::uuid,'language','C++','preference','long_term',$6::bigint,$6::bigint),
      ($3::uuid,$5,$4::uuid,'language','C++','preference','long_term',$6::bigint,$6::bigint)
)sql",
                   std::array{first, viewer, second, other_viewer, scope, time});
        db.execute(R"sql(
INSERT INTO memory_evidence(scope_id,memory_id,source,event_id,quote,occurred_at_ms,day)
VALUES($1,$2::uuid,'fixture','a','我喜欢C++',$4::bigint,0),
      ($1,$3::uuid,'fixture','b','我喜欢C++',$4::bigint,0)
)sql",
                   std::array{scope, first, second, time});
        meowlive::adapters::storage::derive_memory_relationship(db, scope, first, now);
        meowlive::adapters::storage::derive_memory_relationship(db, scope, first, now);
        meowlive::adapters::storage::derive_memory_relationship(db, scope, second, now);
        meowlive::adapters::storage::RelationshipOutbox outbox(url, library, migrations, scope);
        require(outbox.status(now)["pending"] == 2, "Derivation did not enqueue both facts once");
        const auto original = outbox.claim(now);
        const auto companion = outbox.claim(now);
        require(original && companion && !outbox.claim(now), "Lease was granted twice");
        outbox.finish(*companion, now + 1, true);
        const auto retried = outbox.claim(now + 30'000);
        require(retried && retried->token != original->token && retried->id == original->id,
                "Expired lease was not reclaimed");
        outbox.finish(*original, now + 30'001, true);
        require(outbox.status(now + 30'001)["leased"] == 1, "Expired lease acknowledged a retry");
        outbox.finish(*retried, now + 30'001, false);
        require(!outbox.claim(now + 31'000), "Failed projection skipped backoff");
        const auto last = outbox.claim(now + 32'001);
        require(last.has_value(), "Projection recovery never retried");
        outbox.finish(*last, now + 32'002, false);
        require(outbox.status(now + 32'002)["failed"] == 1, "Projection retries were unbounded");

        meowlive::server::GraphWorker worker(url, library, migrations, scope, endpoint, user,
                                             password);
        const auto request = fixture[0].value();
        require(worker.rebuild(request, "Fixture rebuild") == 2 &&
                    worker.rebuild(request, "Fixture rebuild") == 2 &&
                    !worker.rebuild(request, "Different reason"),
                "Rebuild lost idempotency or accepted conflicting parameters");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto status = worker.status();
            if (status["connected"] == true && status["pending"] == 0 && status["leased"] == 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto completed = worker.status();
        require(completed["failed"] == 0 && completed["pending"] == 0 && completed["leased"] == 0 &&
                    worker.connected(),
                "Worker did not reconnect and project retried facts");
        meowlive::adapters::storage::Neo4jProjection projection(endpoint, user, password);
        const auto references = projection.references(scope, viewer, 3000);
        require(references.size() == 1 && references[0u]["id"] == second,
                "Neo4j did not return the connected viewer fact");
        const auto facts = meowlive::adapters::storage::list_viewer_relationships(db, scope, viewer,
                                                                                  references, now);
        require(facts.size() == 2 && facts[0u]["id"] == first && facts[1u]["id"] == second &&
                    facts[0u]["evidence"][0u]["event_id"] == "a",
                "Canonical relationship lookup lost facts or evidence");

        worker.close();
        db.execute("UPDATE memories SET deleted=true,version=version+1,updated_at_ms=$3::bigint"
                   " WHERE scope_id=$1 AND id=$2::uuid",
                   std::array{scope, first, time});
        const auto updated = meowlive::adapters::storage::list_viewer_relationships(
            db, scope, viewer, references, now);
        require(updated.size() == 1 && updated[0u]["deleted"] == true &&
                    updated[0u]["version"] == 2,
                "An invalidated bridging fact exposed stale graph neighbors");
        const auto tombstone = outbox.claim(now);
        require(tombstone && tombstone->id == first && tombstone->fact["deleted"] == true,
                "Memory invalidation did not queue a tombstone");
        projection.project(tombstone->fact, {});
        outbox.finish(*tombstone, now + 1, true);
        auto stale = tombstone->fact;
        stale["deleted"] = false;
        stale["version"] = 1;
        projection.project(stale, {});
        require(projection.references(scope, viewer, 3000).empty(),
                "A stale job revived a tombstone");

        ::Json::Value creation(::Json::objectValue);
        creation["source"]["kind"] = "viewer";
        creation["source"]["id"] = viewer;
        creation["target"]["kind"] = "viewer";
        creation["target"]["id"] = other_viewer;
        creation["kind"] = "friend";
        creation["admin_confirmed"] = false;
        creation["reason"] = "Fixture relationship";
        creation["request_key"] = db.execute("SELECT gen_random_uuid()").at(0).at(0).value();
        creation["evidence"][0u]["source"] = "fixture";
        creation["evidence"][0u]["event_id"] = "a";
        creation["evidence"][0u]["quote"] = "invented";
        require(!meowlive::adapters::storage::edit_relationship(db, scope, "", creation, now),
                "Unverified evidence was accepted");
        creation["evidence"][0u]["quote"] = "C++";
        const auto claim =
            meowlive::adapters::storage::edit_relationship(db, scope, "", creation, now);
        require(claim && (*claim)["confirmation"] == "claimed" &&
                    meowlive::adapters::storage::edit_relationship(db, scope, "", creation, now) ==
                        claim,
                "A verified claim was lost or replayed as a new relation");
        creation["reason"] = "Different creation";
        require(!meowlive::adapters::storage::edit_relationship(db, scope, "", creation, now),
                "A claimed audit key accepted a different creation");
        const auto relation_id = (*claim)["id"].asString();
        ::Json::Value change(::Json::objectValue);
        change["reason"] = "Confirm";
        change["request_key"] = db.execute("SELECT gen_random_uuid()").at(0).at(0).value();
        change["action"] = "confirm";
        change["expected_version"] = 1;
        const auto confirmed =
            meowlive::adapters::storage::edit_relationship(db, scope, relation_id, change, now);
        require(confirmed && (*confirmed)["confirmation"] == "confirmed" &&
                    (*confirmed)["version"] == 2 &&
                    meowlive::adapters::storage::edit_relationship(db, scope, relation_id, change,
                                                                   now) == confirmed,
                "Confirmation failed versioning or idempotency");
        change["request_key"] = db.execute("SELECT gen_random_uuid()").at(0).at(0).value();
        change["action"] = "revoke";
        require(
            !meowlive::adapters::storage::edit_relationship(db, scope, relation_id, change, now),
            "A stale administrator edit overwrote a newer fact");
        change["expected_version"] = 2;
        const auto revoked =
            meowlive::adapters::storage::edit_relationship(db, scope, relation_id, change, now);
        require(revoked && (*revoked)["confirmation"] == "claimed" && (*revoked)["version"] == 3,
                "Revocation did not produce a claimed projection version");
        change["request_key"] = db.execute("SELECT gen_random_uuid()").at(0).at(0).value();
        change["action"] = "delete";
        change["expected_version"] = 3;
        const auto removed =
            meowlive::adapters::storage::edit_relationship(db, scope, relation_id, change, now);
        require(removed && (*removed)["deleted"] == true && (*removed)["version"] == 4,
                "Deletion did not leave a versioned tombstone");
        const auto manual_job = outbox.claim(now);
        require(manual_job && manual_job->id == relation_id && manual_job->fact["version"] == 4,
                "Manual edits did not durably queue the latest projection");

        ::Json::Value parameters(::Json::objectValue);
        parameters["scope"] = scope;
        projection.query("MATCH (n {scope:$scope}) DETACH DELETE n", parameters, {});
    }
}

int main(int argc, char** argv)
{
    try
    {
        verify(argc > 1 ? argv[1] : "adapters/storage/migrations", argc > 2 ? argv[2] : "");
        std::puts("PostgreSQL and Neo4j relationship projection passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
