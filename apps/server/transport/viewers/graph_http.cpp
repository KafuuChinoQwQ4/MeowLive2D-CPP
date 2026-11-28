module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.graph_http;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.domain.speech;
import meowlive.protocol.json;
import meowlive.server.agent_worker;
import meowlive.server.graph_worker;
import meowlive.server.transport.access;
import meowlive.server.transport.media_type;
import meowlive.server.transport.storage_http;
import meowlive.server.viewer_admin;

namespace
{
    constexpr std::string_view graph_status_path{"/api/admin/graph/status"};
    constexpr std::string_view graph_rebuild_path{"/api/admin/graph/rebuild"};
    constexpr std::string_view graph_viewer_prefix{"/api/admin/viewers/"};
    constexpr std::string_view graph_viewer_suffix{"/relationships"};
    constexpr std::string_view graph_facts_path{"/api/admin/relationships"};
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;

    struct RelationshipKnowledgeEdit
    {
        explicit RelationshipKnowledgeEdit(meowlive::server::AgentWorker& worker) : agent(worker)
        {
            agent.begin_knowledge_edit();
        }

        ~RelationshipKnowledgeEdit()
        {
            agent.end_knowledge_edit();
        }

        meowlive::server::AgentWorker& agent;
    };

    drogon::HttpResponsePtr graph_unavailable()
    {
        return meowlive::server::transport::error_response(
            drogon::k503ServiceUnavailable, "graph_unavailable", "关系图存储当前不可用");
    }

    drogon::HttpResponsePtr graph_json(::Json::Value value)
    {
        auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(value));
        response->addHeader("cache-control", "no-store");
        return response;
    }

    bool graph_uuid(std::string_view key)
    {
        if (key.size() != 36)
        {
            return false;
        }
        constexpr std::string_view hex{"0123456789abcdefABCDEF"};
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            const auto separator = index == 8 || index == 13 || index == 18 || index == 23;
            if (separator ? key[index] != '-' : !hex.contains(key[index]))
            {
                return false;
            }
        }
        return true;
    }

    bool valid_graph_rebuild(const ::Json::Value& body)
    {
        if (body.size() != 2 || !body["request_key"].isString() || !body["reason"].isString() ||
            !graph_uuid(body["request_key"].asString()))
        {
            return false;
        }
        const auto reason = body["reason"].asString();
        return meowlive::domain::normalize_speech_text(reason, 1000).has_value() &&
               reason.find_first_of("\t\r\n") == std::string::npos;
    }

    bool valid_relationship_text(const ::Json::Value& value, std::size_t limit)
    {
        if (!value.isString())
        {
            return false;
        }
        const auto text = value.asString();
        return meowlive::domain::normalize_speech_text(text, limit).has_value() &&
               text.find_first_of("\t\r\n") == std::string::npos;
    }

    bool valid_relationship_creation(const ::Json::Value& body)
    {
        const auto& entity = body["source"];
        const auto& target = body["target"];
        if (body.size() != (body.isMember("expires_at_ms") ? 8u : 7u) || !entity.isObject() ||
            entity.size() != 2 || entity["kind"] != "viewer" || !entity["id"].isString() ||
            !graph_uuid(entity["id"].asString()) || !target.isObject() || target.size() != 2 ||
            !valid_relationship_text(target["id"], 128) || !target["kind"].isString() ||
            !body["kind"].isString() || !body["admin_confirmed"].isBool() ||
            !body["evidence"].isArray() || body["evidence"].size() > 8 ||
            (!body["expires_at_ms"].isNull() &&
             (!body["expires_at_ms"].isInt64() || body["expires_at_ms"].asInt64() < 1)))
        {
            return false;
        }
        const auto target_kind = target["kind"].asString();
        const auto kind = body["kind"].asString();
        if ((target_kind != "topic" && target_kind != "activity" && target_kind != "unresolved" &&
             target_kind != "viewer") ||
            (target_kind == "viewer" && !graph_uuid(target["id"].asString())) ||
            !(kind == "mention" || (kind == "shared_interest" && target_kind == "topic") ||
              (kind == "participated" && target_kind == "activity") ||
              ((kind == "friend" || kind == "acquaintance") && target_kind == "viewer")))
        {
            return false;
        }
        for (const auto& proof : body["evidence"])
        {
            if (!proof.isObject() || proof.size() != 3 ||
                !valid_relationship_text(proof["source"], 100) ||
                !valid_relationship_text(proof["event_id"], 255) ||
                !valid_relationship_text(proof["quote"], 300))
            {
                return false;
            }
        }
        return body["admin_confirmed"].asBool() || !body["evidence"].empty();
    }

    bool graph_fact_path(std::string_view path)
    {
        return path == graph_facts_path ||
               (path.starts_with(graph_facts_path) && path.size() > graph_facts_path.size() + 1 &&
                path[graph_facts_path.size()] == '/');
    }

    void respond_relationship_edit(const drogon::HttpRequestPtr& request,
                                   const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                                   const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                                   meowlive::application::WorkQueue& work, const Send& send)
    {
        auto path = std::string_view(request->getOriginalPath());
        path.remove_prefix(graph_facts_path.size());
        const bool create = path.empty();
        if (!create)
        {
            path.remove_prefix(1);
        }
        std::optional<::Json::Value> body;
        if (request->body().size() <= 16 * 1024 &&
            meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            body = meowlive::protocol::json::parse_object(request->body());
        }
        if (!body || !(*body)["request_key"].isString() ||
            !graph_uuid((*body)["request_key"].asString()) ||
            !valid_relationship_text((*body)["reason"], 1000) ||
            (create ? !valid_relationship_creation(*body)
                    : !graph_uuid(path) || body->size() != 4 ||
                          !(*body)["expected_version"].isInt64() ||
                          (*body)["expected_version"].asInt64() < 1 ||
                          ((*body)["action"] != "confirm" && (*body)["action"] != "revoke" &&
                           (*body)["action"] != "delete")))
        {
            send(meowlive::server::transport::error_response(
                drogon::k400BadRequest, "invalid_relationship", "关系标识、类型或操作依据无效"));
            return;
        }
        if (!viewers || !viewers->enabled())
        {
            send(graph_unavailable());
            return;
        }
        meowlive::server::transport::submit_storage_http(
            work, create ? graph_facts_path : "/api/admin/relationships/{id}", send,
            [viewers, agent, id = std::string(path), body = std::move(*body)]
            {
                const RelationshipKnowledgeEdit edit(*agent);
                const auto result = viewers->edit_relationship(id, body);
                return result ? graph_json(*result)
                              : meowlive::server::transport::error_response(
                                    drogon::k409Conflict, "relationship_conflict",
                                    "操作未完成，请核对证据并刷新版本");
            },
            graph_unavailable);
    }

    bool graph_relationship_path(std::string_view path)
    {
        return path.size() > graph_viewer_prefix.size() + graph_viewer_suffix.size() &&
               path.starts_with(graph_viewer_prefix) && path.ends_with(graph_viewer_suffix);
    }

    void respond_relationships(const drogon::HttpRequestPtr& request,
                               const std::shared_ptr<meowlive::server::GraphWorker>& graph,
                               const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                               meowlive::application::WorkQueue& work, const Send& send)
    {
        auto path = std::string_view(request->getOriginalPath());
        path.remove_prefix(graph_viewer_prefix.size());
        path.remove_suffix(graph_viewer_suffix.size());
        const auto& parameters = request->getParameters();
        const auto depth = parameters.find("depth");
        if (!graph_uuid(path) ||
            (!parameters.empty() && (parameters.size() != 1 || depth == parameters.end() ||
                                     (depth->second != "1" && depth->second != "2"))))
        {
            send(meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                             "invalid_relationship_operation",
                                                             "观众标识或关系深度无效"));
            return;
        }
        if (!viewers || !viewers->enabled())
        {
            send(graph_unavailable());
            return;
        }
        const bool two_hops = depth != parameters.end() && depth->second == "2";
        meowlive::server::transport::submit_storage_http(
            work, "/api/admin/viewers/{id}/relationships", send,
            [graph, viewers, id = std::string(path), two_hops]
            {
                std::optional<::Json::Value> references;
                if (two_hops && graph)
                {
                    references = graph->references(id);
                }
                ::Json::Value result(::Json::objectValue);
                result["degraded"] = two_hops ? !references : !graph || !graph->connected();
                result["relationships"] =
                    viewers->relationships(id, references.value_or(::Json::arrayValue));
                return graph_json(std::move(result));
            },
            graph_unavailable);
    }
}

namespace meowlive::server::transport
{
    std::array<std::string_view, 3> graph_paths()
    {
        return std::array{graph_status_path, graph_rebuild_path, graph_facts_path};
    }

    std::string_view graph_route_methods(std::string_view path)
    {
        if (path == graph_status_path || graph_relationship_path(path))
        {
            return "GET,HEAD";
        }
        return path == graph_rebuild_path || graph_fact_path(path) ? "POST" : "";
    }

    void respond_graph(const drogon::HttpRequestPtr& request,
                       const std::shared_ptr<meowlive::server::GraphWorker>& graph,
                       const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                       const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                       meowlive::application::WorkQueue& work, Send send)
    {
        if (graph_relationship_path(request->getOriginalPath()))
        {
            respond_relationships(request, graph, viewers, work, send);
            return;
        }
        if (graph_fact_path(request->getOriginalPath()))
        {
            respond_relationship_edit(request, viewers, agent, work, send);
            return;
        }
        const bool rebuild = request->getOriginalPath() == graph_rebuild_path;
        std::optional<::Json::Value> body;
        if (rebuild)
        {
            if (request->body().size() <= 16 * 1024 &&
                meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
            {
                body = meowlive::protocol::json::parse_object(request->body());
            }
            if (!body || !valid_graph_rebuild(*body))
            {
                send(meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "invalid_graph_operation", "重建请求标识或依据无效"));
                return;
            }
        }
        if (!graph)
        {
            if (rebuild)
            {
                send(graph_unavailable());
            }
            else
            {
                ::Json::Value result(::Json::objectValue);
                result["enabled"] = false;
                result["connected"] = false;
                result["pending"] = 0;
                result["leased"] = 0;
                result["failed"] = 0;
                result["oldest_pending_age_ms"] = ::Json::Value{};
                send(graph_json(std::move(result)));
            }
            return;
        }
        meowlive::server::transport::submit_storage_http(
            work, rebuild ? graph_rebuild_path : graph_status_path, send,
            [graph, body = std::move(body)]
            {
                if (!body)
                {
                    return graph_json(graph->status());
                }
                const auto queued =
                    graph->rebuild((*body)["request_key"].asString(), (*body)["reason"].asString());
                if (!queued)
                {
                    return meowlive::server::transport::error_response(
                        drogon::k409Conflict, "graph_operation_conflict", "图重建操作冲突");
                }
                ::Json::Value result(::Json::objectValue);
                result["queued"] = static_cast<::Json::UInt64>(*queued);
                return graph_json(std::move(result));
            },
            graph_unavailable);
    }
}
