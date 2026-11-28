module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.agent_http;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.agent_runtime_settings;
import meowlive.server.agent_json;
import meowlive.server.agent_worker;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;

namespace
{
    constexpr std::string_view detail_prefix{"/api/agent/traces/"};

    std::uint64_t parsed_time(const std::string& parameter, std::uint64_t fallback)
    {
        std::uint64_t value{};
        const auto end = parameter.data() + parameter.size();
        const auto result = std::from_chars(parameter.data(), end, value);
        return result.ptr == end && result.ec == std::errc{} ? value : fallback;
    }

    drogon::HttpResponsePtr json_response(::Json::Value value,
                                          drogon::HttpStatusCode status = drogon::k200OK)
    {
        auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(value));
        response->setStatusCode(status);
        response->addHeader("cache-control", "no-store");
        return response;
    }
}

export namespace meowlive::server::transport
{
    auto agent_paths()
    {
        return std::array<std::string_view, 9>{
            "/api/agent",        "/api/agent/pause",    "/api/agent/resume",
            "/api/events",       "/api/agent/activity", "/api/agent/scheduler",
            "/api/agent/traces", "/api/llm/usage",      "/api/agent/runtime"};
    }

    std::string_view agent_route_methods(std::string_view path)
    {
        if (path == "/api/agent/runtime")
        {
            return "GET,HEAD,POST";
        }
        if (path == "/api/events" || path == "/api/agent/pause" || path == "/api/agent/resume")
        {
            return "POST";
        }
        if (path == "/api/agent" || path == "/api/agent/activity" ||
            path == "/api/agent/scheduler" || path == "/api/agent/traces" ||
            path == "/api/llm/usage" || path.starts_with(detail_prefix))
        {
            return "GET,HEAD";
        }
        return {};
    }

    void respond_runtime(const drogon::HttpRequestPtr& request,
                         meowlive::server::AgentRuntimeSettings& runtime,
                         meowlive::application::WorkQueue& queue,
                         std::function<void(const drogon::HttpResponsePtr&)> send)
    {
        if (request->method() == drogon::Get)
        {
            send(json_response(runtime.snapshot()));
            return;
        }
        if (request->body().size() <= 512 * 1024 &&
            meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            if (auto parsed = meowlive::protocol::json::parse_object(request->body()))
            {
                meowlive::server::transport::submit_http_work(
                    queue, "/api/agent/runtime", send,
                    [&runtime, body = std::move(*parsed)]
                    {
                        const auto saved = runtime.save(body);
                        if (saved)
                        {
                            return json_response(runtime.snapshot());
                        }
                        if (saved.error() == meowlive::server::AgentRuntimeSaveError::Storage)
                        {
                            return meowlive::server::transport::error_response(
                                drogon::k500InternalServerError, "agent_runtime_save_failed",
                                "无法保存本机运行设置");
                        }
                        return meowlive::server::transport::error_response(
                            drogon::k400BadRequest, "invalid_agent_runtime", "运行设置无效");
                    },
                    "settings_busy", "本机设置队列繁忙",
                    meowlive::platform::ExceptionDetails::Omit);
                return;
            }
        }
        send(meowlive::server::transport::error_response(
            drogon::k400BadRequest, "invalid_agent_runtime", "运行设置格式或大小无效"));
    }

    drogon::HttpResponsePtr respond_agent(const drogon::HttpRequestPtr& request,
                                          meowlive::server::AgentWorker& worker)
    {
        const std::string_view path = request->getOriginalPath();
        if (path == "/api/events")
        {
            if (request->body().size() <= 256 * 1024 &&
                meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
            {
                if (const auto body = meowlive::protocol::json::parse_object(request->body()))
                {
                    if (auto events = meowlive::server::agent_json::events(*body))
                    {
                        const auto counts = worker.submit(std::move(*events));
                        ::Json::Value result(::Json::objectValue);
                        result["accepted"] = counts.accepted;
                        result["duplicates"] = counts.duplicates;
                        if (counts.unscheduled)
                        {
                            result["unscheduled"] = counts.unscheduled;
                        }
                        return json_response(std::move(result), drogon::k202Accepted);
                    }
                }
            }
            return meowlive::server::transport::error_response(
                drogon::k400BadRequest, "invalid_agent_request", "请求格式、字段或大小无效");
        }
        if (path == "/api/agent/pause")
        {
            worker.pause();
        }
        if (path == "/api/agent/resume")
        {
            if (const auto error = worker.resume(); !error.empty())
            {
                const auto message = error == "llm_unconfigured"         ? "请先配置模型并重启服务"
                                     : error == "bridge_disconnected"    ? "执行端未连接"
                                     : error == "knowledge_edit_pending" ? "观众资料正在更新"
                                                                         : "尚未选择可用音色";
                return meowlive::server::transport::error_response(drogon::k409Conflict, error,
                                                                   message);
            }
        }
        if (path == "/api/agent/scheduler")
        {
            return json_response(worker.scheduler());
        }
        if (path == "/api/agent/activity")
        {
            return json_response(worker.activity());
        }
        if (path == "/api/agent/traces")
        {
            return json_response(worker.traces());
        }
        if (path.starts_with(detail_prefix))
        {
            auto trace = worker.trace(std::string(path.substr(detail_prefix.size())));
            if (trace.isNull())
            {
                return meowlive::server::transport::error_response(
                    drogon::k404NotFound, "trace_not_found", "未找到决策记录");
            }
            return json_response(std::move(trace));
        }
        if (path == "/api/llm/usage")
        {
            return json_response(
                worker.usage(request->getParameter("provider"), request->getParameter("model"),
                             parsed_time(request->getParameter("since_ms"), 0),
                             parsed_time(request->getParameter("until_ms"),
                                         (std::numeric_limits<std::uint64_t>::max)())));
        }
        return json_response(worker.snapshot());
    }
}
