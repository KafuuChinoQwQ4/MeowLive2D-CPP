module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.memory_http;

import meowlive.standard;

import meowlive.adapters.storage.memory_admin;
import meowlive.application.work_queue;
import meowlive.protocol.json;
import meowlive.server.agent_worker;
import meowlive.server.transport.access;
import meowlive.server.transport.media_type;
import meowlive.server.transport.storage_http;

namespace
{
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;
    constexpr std::string_view viewer_prefix{"/api/admin/viewers/"};
    constexpr std::string_view memory_suffix{"/memories"};
    constexpr std::string_view memory_status{"/api/admin/memories/status"};
    constexpr std::string_view memory_retry{"/api/admin/memories/retry"};
    constexpr std::string_view memory_rebuild{"/api/admin/memories/rebuild-vectors"};

    bool uuid(std::string_view value)
    {
        if (value.size() != 36)
        {
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            const auto c = value[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != '-')
                {
                    return false;
                }
            }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            {
                return false;
            }
        }
        return true;
    }

    std::string_view viewer_path(std::string_view path)
    {
        if (!path.starts_with(viewer_prefix))
        {
            return {};
        }
        path.remove_prefix(viewer_prefix.size());
        return path.size() >= 36 && uuid(path.substr(0, 36)) &&
                       path.substr(36).starts_with(memory_suffix)
                   ? path
                   : std::string_view{};
    }

    bool valid_text(const ::Json::Value& input, std::size_t limit)
    {
        if (!input.isString())
        {
            return false;
        }
        const auto value = input.asString();
        return value.size() <= limit && value.find('\0') == std::string::npos &&
               value.find_first_not_of(" \t\r\n") != std::string::npos;
    }

    drogon::HttpResponsePtr invalid()
    {
        return meowlive::server::transport::error_response(
            drogon::k400BadRequest, "invalid_memory_operation", "记忆操作的字段无效");
    }

    drogon::HttpResponsePtr unavailable()
    {
        return meowlive::server::transport::error_response(
            drogon::k503ServiceUnavailable, "memory_unavailable", "记忆存储当前不可用");
    }

    drogon::HttpResponsePtr success(::Json::Value response)
    {
        auto result = drogon::HttpResponse::newHttpJsonResponse(std::move(response));
        result->addHeader("cache-control", "no-store");
        return result;
    }

    drogon::HttpResponsePtr update_result(bool updated)
    {
        if (!updated)
        {
            return meowlive::server::transport::error_response(
                drogon::k409Conflict, "memory_version_conflict", "记忆版本或请求标识已变更");
        }
        ::Json::Value response(::Json::objectValue);
        response["updated"] = true;
        return success(std::move(response));
    }

    struct KnowledgeEdit
    {
        explicit KnowledgeEdit(meowlive::server::AgentWorker& worker) : agent(worker)
        {
            agent.begin_knowledge_edit();
        }
        ~KnowledgeEdit()
        {
            agent.end_knowledge_edit();
        }
        meowlive::server::AgentWorker& agent;
    };

    drogon::HttpResponsePtr process(meowlive::adapters::storage::MemoryAdmin& repository,
                                    meowlive::server::AgentWorker& agent, std::string_view path,
                                    const ::Json::Value& input)
    {
        const auto now =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        if (path == memory_status)
        {
            return success(repository.status());
        }
        const auto resource = viewer_path(path);
        if (resource.size() == 36 + memory_suffix.size())
        {
            return success(repository.list(std::string(resource.substr(0, 36)), now));
        }
        if (!input["request_key"].isString() || !uuid(input["request_key"].asString()) ||
            !valid_text(input["reason"], 1000))
        {
            return invalid();
        }
        const auto key = input["request_key"].asString();
        const auto reason = input["reason"].asString();
        if (path == memory_retry || path == memory_rebuild)
        {
            if (input.size() != 2)
            {
                return invalid();
            }
            if (path == memory_rebuild && !repository.vectors_enabled())
            {
                return meowlive::server::transport::error_response(
                    drogon::k409Conflict, "memory_embeddings_disabled", "请先配置向量模型");
            }
            return update_result(repository.maintain(key, reason, path == memory_rebuild, now));
        }

        if (!input["operation"].isString() || !input["expected_version"].isInt64() ||
            input["expected_version"].asInt64() < 1 ||
            input["expected_version"].asInt64() == (std::numeric_limits<std::int64_t>::max)() ||
            !std::ranges::all_of(input.getMemberNames(),
                                 [](const auto& name)
                                 {
                                     return name == "request_key" || name == "reason" ||
                                            name == "operation" || name == "expected_version" ||
                                            name == "value" || name == "frozen";
                                 }))
        {
            return invalid();
        }
        auto operation = input["operation"].asString();
        std::string value;
        if (operation == "correct" && valid_text(input["value"], 512))
        {
            value = input["value"].asString();
        }
        else if (operation == "freeze" && input["frozen"].isBool())
        {
            operation = input["frozen"].asBool() ? "freeze" : "unfreeze";
        }
        else if (operation != "delete")
        {
            return invalid();
        }
        const KnowledgeEdit edit(agent);
        return update_result(
            repository.mutate(std::string(resource.substr(0, 36)),
                              std::string(resource.substr(37 + memory_suffix.size())), key, reason,
                              operation, value, input["expected_version"].asInt64(), now));
    }
}

namespace meowlive::server::transport
{
    std::string_view memory_route_methods(std::string_view path)
    {
        if (path == memory_status)
        {
            return "GET,HEAD";
        }
        if (path == memory_retry || path == memory_rebuild)
        {
            return "POST";
        }
        const auto resource = viewer_path(path);
        if (resource.size() == 36 + memory_suffix.size())
        {
            return "GET,HEAD";
        }
        if (resource.size() > 36 + memory_suffix.size() &&
            resource[36 + memory_suffix.size()] == '/' &&
            uuid(resource.substr(37 + memory_suffix.size())))
        {
            return "POST";
        }
        return {};
    }

    void respond_memory(const drogon::HttpRequestPtr& request,
                        const std::shared_ptr<meowlive::adapters::storage::MemoryAdmin>& repository,
                        const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                        meowlive::application::WorkQueue& work, Send send)
    {
        if (!repository)
        {
            send(unavailable());
            return;
        }

        ::Json::Value input(::Json::objectValue);
        if (request->method() == drogon::Post)
        {
            if (request->body().size() > 16 * 1024 ||
                !meowlive::server::transport::is_json_media_type(
                    request->getHeader("content-type")))
            {
                send(invalid());
                return;
            }
            auto parsed = meowlive::protocol::json::parse_object(request->body());
            if (!parsed)
            {
                send(invalid());
                return;
            }
            input = std::move(*parsed);
        }
        meowlive::server::transport::submit_storage_http(
            work, request->getMatchedPathPattern(), send,
            [repository, agent, path = request->getOriginalPath(), input = std::move(input)]
            {
                return process(*repository, *agent, path, input);
            },
            unavailable);
    }
}
