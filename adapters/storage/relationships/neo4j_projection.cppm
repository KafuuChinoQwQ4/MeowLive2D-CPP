module;

#include <curl/curl.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.adapters.storage.neo4j_projection;

import meowlive.standard;

import meowlive.adapters.http.curl_resources;
import meowlive.adapters.http.response_body;
import meowlive.protocol.json;

export namespace meowlive::adapters::storage
{
    class Neo4jError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
        ~Neo4jError() override;
    };

    class Neo4jUnavailable : public meowlive::adapters::storage::Neo4jError
    {
    public:
        using meowlive::adapters::storage::Neo4jError::Neo4jError;
        ~Neo4jUnavailable() override;
    };

    class Neo4jCancelled : public meowlive::adapters::storage::Neo4jError
    {
    public:
        using meowlive::adapters::storage::Neo4jError::Neo4jError;
        ~Neo4jCancelled() override;
    };
}

namespace meowlive::adapters::storage
{
    Neo4jError::~Neo4jError() = default;
    Neo4jUnavailable::~Neo4jUnavailable() = default;
    Neo4jCancelled::~Neo4jCancelled() = default;
}

namespace
{
    constexpr auto projection_cypher = R"cypher(
MERGE (f:MeowFact {scope:$scope,id:$id})
ON CREATE SET f.version=0, f.lock=0
SET f.lock=coalesce(f.lock,0)+1
WITH f WHERE f.version < $version
SET f.version=$version,f.deleted=$deleted,f.confirmed=$confirmed,f.kind=$kind,f.expires=$expires
WITH f OPTIONAL MATCH (f)-[old:MEOW_SOURCE|MEOW_TARGET]->()
DELETE old
WITH DISTINCT f
FOREACH (_ IN CASE WHEN NOT $deleted AND $confirmed THEN [1] ELSE [] END |
    MERGE (a:MeowEntity {scope:$scope,kind:$source_kind,id:$source_id})
    MERGE (b:MeowEntity {scope:$scope,kind:$target_kind,id:$target_id})
    MERGE (f)-[:MEOW_SOURCE]->(a)
    MERGE (f)-[:MEOW_TARGET]->(b))
RETURN f.id AS id
)cypher";

    constexpr auto neighborhood_cypher = R"cypher(
MATCH (viewer:MeowEntity {scope:$scope,kind:'viewer',id:$viewer})
 <-[:MEOW_SOURCE|MEOW_TARGET]-(first:MeowFact)-[:MEOW_SOURCE|MEOW_TARGET]->(middle:MeowEntity)
 <-[:MEOW_SOURCE|MEOW_TARGET]-(fact:MeowFact)
WHERE first.scope=$scope AND middle.scope=$scope AND fact.scope=$scope
  AND NOT first.deleted AND first.confirmed AND (first.expires IS NULL OR first.expires>timestamp())
  AND NOT fact.deleted AND fact.confirmed AND (fact.expires IS NULL OR fact.expires>timestamp())
RETURN DISTINCT fact.id AS id,fact.version AS version
ORDER BY id LIMIT 40
)cypher";

    void check_url(::CURLUcode code)
    {
        if (meowlive::adapters::http::check_curl_memory(code) != CURLUE_OK)
        {
            throw meowlive::adapters::storage::Neo4jError("neo4j_endpoint_invalid");
        }
    }

    void check_curl_setup(::CURLcode code)
    {
        if (meowlive::adapters::http::check_curl_memory(code) != CURLE_OK)
        {
            throw meowlive::adapters::storage::Neo4jError(std::string("neo4j_curl_setup_failed: ") +
                                                          ::curl_easy_strerror(code));
        }
    }

    void check_transfer(::CURLcode code, std::stop_token stop)
    {
        if (stop.stop_requested() && code == CURLE_ABORTED_BY_CALLBACK)
        {
            throw meowlive::adapters::storage::Neo4jCancelled("neo4j_query_cancelled");
        }
        if (meowlive::adapters::http::check_curl_memory(code) == CURLE_OK)
        {
            return;
        }
        const auto message = std::string("neo4j_transfer_failed: ") + ::curl_easy_strerror(code);
        switch (code)
        {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
            throw meowlive::adapters::storage::Neo4jUnavailable(message);
        default:
            throw meowlive::adapters::storage::Neo4jError(message);
        }
    }

    void check_query_errors(const ::Json::Value& errors)
    {
        if (errors.empty())
        {
            return;
        }
        if (errors.isArray() &&
            std::ranges::all_of(errors,
                                [](const ::Json::Value& error)
                                {
                                    return error.isObject() && error["code"].isString() &&
                                           std::string_view(error["code"].asCString())
                                               .starts_with("Neo.TransientError.");
                                }))
        {
            throw meowlive::adapters::storage::Neo4jUnavailable("neo4j_query_transient_error");
        }
        throw meowlive::adapters::storage::Neo4jError("neo4j_query_rejected");
    }

    std::string normalized_graph_url(const std::string& input)
    {
        if (input.size() > 2048 || input.find('\0') != std::string::npos ||
            input.find_first_of("?#\\\r\n\t ") != std::string::npos)
        {
            throw meowlive::adapters::storage::Neo4jError("neo4j_endpoint_invalid");
        }
        const std::unique_ptr<::CURLU, decltype(&::curl_url_cleanup)> parsed(::curl_url(),
                                                                             ::curl_url_cleanup);
        if (!parsed)
        {
            throw std::bad_alloc{};
        }
        check_url(::curl_url_set(parsed.get(), CURLUPART_URL, input.c_str(), CURLU_DISALLOW_USER));
        check_url(::curl_url_set(parsed.get(), CURLUPART_PATH, "/db/neo4j/query/v2", 0));
        char* value{};
        check_url(::curl_url_get(parsed.get(), CURLUPART_URL, &value, 0));
        const std::unique_ptr<char, decltype(&::curl_free)> url(value, ::curl_free);
        std::string result(url.get());
        if (!result.starts_with("http://") && !result.starts_with("https://"))
        {
            throw meowlive::adapters::storage::Neo4jError("neo4j_endpoint_invalid");
        }
        return result;
    }
}

export namespace meowlive::adapters::storage
{
    class Neo4jProjection
    {
    public:
        Neo4jProjection(const std::string& endpoint, std::string username, std::string password)
            : endpoint_(normalized_graph_url(endpoint)), username_(std::move(username)),
              password_(std::move(password))
        {
            if (username_.empty() || password_.empty())
            {
                throw meowlive::adapters::storage::Neo4jError("neo4j_credentials_missing");
            }
        }

        ::Json::Value query(std::string_view statement, const ::Json::Value& parameters,
                            std::stop_token stop, long timeout_ms = 10'000) const
        {
            static const auto initialized = ::curl_global_init(CURL_GLOBAL_DEFAULT);
            check_curl_setup(initialized);
            if (stop.stop_requested())
            {
                throw meowlive::adapters::storage::Neo4jCancelled("neo4j_query_cancelled");
            }
            ::Json::Value payload(::Json::objectValue);
            payload["statement"] = std::string(statement);
            payload["parameters"] = parameters;
            const auto request = meowlive::protocol::json::serialize(payload);
            meowlive::adapters::http::ResponseBody response{64 * 1024};
            const std::unique_ptr<::CURL, decltype(&::curl_easy_cleanup)> easy(::curl_easy_init(),
                                                                               ::curl_easy_cleanup);
            const std::unique_ptr<::curl_slist, decltype(&::curl_slist_free_all)> headers(
                ::curl_slist_append(nullptr, "Content-Type: application/json"),
                ::curl_slist_free_all);
            if (!easy || !headers)
            {
                throw std::bad_alloc{};
            }
            const auto option = [&](auto setting, auto value)
            {
                check_curl_setup(::curl_easy_setopt(easy.get(), setting, value));
            };
            option(CURLOPT_URL, endpoint_.c_str());
            option(CURLOPT_PROTOCOLS_STR, "http,https");
            option(CURLOPT_PROXY, "");
            option(CURLOPT_NOSIGNAL, 1L);
            option(CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            option(CURLOPT_USERNAME, username_.c_str());
            option(CURLOPT_PASSWORD, password_.c_str());
            option(CURLOPT_HTTPHEADER, headers.get());
            option(CURLOPT_CONNECTTIMEOUT, 3L);
            option(CURLOPT_TIMEOUT_MS, timeout_ms);
            option(CURLOPT_POSTFIELDS, request.data());
            option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<::curl_off_t>(request.size()));
            option(CURLOPT_WRITEFUNCTION, meowlive::adapters::http::ResponseBody::receive);
            option(CURLOPT_WRITEDATA, &response);
            option(CURLOPT_NOPROGRESS, 0L);
            option(CURLOPT_XFERINFODATA, &stop);
            option(
                CURLOPT_XFERINFOFUNCTION,
                +[](void* token, ::curl_off_t, ::curl_off_t, ::curl_off_t, ::curl_off_t) noexcept
                {
                    return static_cast<std::stop_token*>(token)->stop_requested() ? 1 : 0;
                });
            const auto code = ::curl_easy_perform(easy.get());
            response.rethrow_failure();
            if (response.exceeded())
            {
                throw meowlive::adapters::storage::Neo4jError("neo4j_response_too_large");
            }
            check_transfer(code, stop);
            long status{};
            check_curl_setup(::curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status));
            if (status < 200 || status >= 300)
            {
                const auto message = "neo4j_http_" + std::to_string(status);
                if (status == 408 || status == 429 || status == 502 || status == 503 ||
                    status == 504)
                {
                    throw meowlive::adapters::storage::Neo4jUnavailable(message);
                }
                throw meowlive::adapters::storage::Neo4jError(message);
            }
            const auto parsed = meowlive::protocol::json::parse_object(response.bytes());
            if (!parsed)
            {
                throw meowlive::adapters::storage::Neo4jError("neo4j_response_invalid");
            }
            check_query_errors((*parsed)["errors"]);
            if (!(*parsed)["data"].isObject())
            {
                throw meowlive::adapters::storage::Neo4jError("neo4j_response_invalid");
            }
            return (*parsed)["data"];
        }

        void prepare(std::stop_token stop) const
        {
            for (const std::string_view statement :
                 {"CREATE CONSTRAINT meowlive_fact_key IF NOT EXISTS FOR (f:MeowFact) REQUIRE "
                  "(f.scope, f.id) IS UNIQUE",
                  "CREATE CONSTRAINT meowlive_entity_key IF NOT EXISTS FOR (e:MeowEntity) REQUIRE "
                  "(e.scope, e.kind, e.id) IS UNIQUE"})
            {
                query(statement, ::Json::Value(::Json::objectValue), stop);
            }
        }

        void project(const ::Json::Value& fact, std::stop_token stop) const
        {
            query(projection_cypher, fact, stop);
        }

        ::Json::Value references(const std::string& scope, const std::string& viewer,
                                 long timeout_ms = 250) const
        {
            ::Json::Value parameters(::Json::objectValue);
            parameters["scope"] = scope;
            parameters["viewer"] = viewer;
            const auto data = query(neighborhood_cypher, parameters, std::stop_token{}, timeout_ms);
            const auto& fields = data["fields"];
            const auto& rows = data["values"];
            if (!fields.isArray() || fields.size() != 2 || fields[0u] != "id" ||
                fields[1u] != "version" || !rows.isArray() || rows.size() > 40)
            {
                throw meowlive::adapters::storage::Neo4jError("neo4j_references_invalid");
            }
            ::Json::Value result(::Json::arrayValue);
            for (const auto& row : rows)
            {
                if (!row.isArray() || row.size() != 2 || !row[0u].isString() ||
                    row[0u].asString().size() != 36 || !row[1u].isInt64() || row[1u].asInt64() < 1)
                {
                    throw meowlive::adapters::storage::Neo4jError("neo4j_references_invalid");
                }
                ::Json::Value reference(::Json::objectValue);
                reference["id"] = row[0u];
                reference["version"] = row[1u];
                result.append(std::move(reference));
            }
            return result;
        }

    private:
        const std::string endpoint_;
        const std::string username_;
        const std::string password_;
    };
}
