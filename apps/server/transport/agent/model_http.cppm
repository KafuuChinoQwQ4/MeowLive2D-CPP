module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.model_http;

import meowlive.standard;

import meowlive.adapters.llm.language_model;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.json;
import meowlive.server.agent_model;
import meowlive.server.profile_settings;
import meowlive.server.transport.access;
import meowlive.server.transport.http_work;
import meowlive.server.transport.media_type;

namespace
{
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;

    drogon::HttpResponsePtr invalid_model()
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_llm_settings", "模型配置无效");
    }

    drogon::HttpResponsePtr provider_error(const std::string& message)
    {
        return meowlive::server::transport::error_response(drogon::k502BadGateway,
                                                           "llm_request_failed", message);
    }
}

export namespace meowlive::server::transport
{
    auto model_paths()
    {
        return std::array<std::string_view, 3>{"/api/llm/models", "/api/llm/test",
                                               "/api/llm/reasoning"};
    }

    std::string_view model_route_methods(std::string_view path)
    {
        return path == "/api/llm/models" || path == "/api/llm/test" || path == "/api/llm/reasoning"
                   ? "POST"
                   : "";
    }

    void respond_model(const drogon::HttpRequestPtr& request,
                       meowlive::server::ProfileSettings& profiles,
                       meowlive::application::WorkQueue& queue, std::stop_token stop, Send send)
    {
        if (request->body().size() > 16 * 1024 ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            send(invalid_model());
            return;
        }
        auto input = meowlive::protocol::json::parse_object(request->body());
        if (!input)
        {
            send(invalid_model());
            return;
        }
        const std::string_view path = request->getOriginalPath();
        if (path == "/api/llm/reasoning")
        {
            for (const auto* field : {"provider", "api_format", "model", "reasoning_effort"})
            {
                if (!(*input)[field].isString())
                {
                    send(invalid_model());
                    return;
                }
            }
            ::Json::Value result(::Json::objectValue);
            result["requested"] = (*input)["reasoning_effort"];
            result["effective"] = ::Json::Value{};
            result["supported"] = ::Json::Value(::Json::arrayValue);
            result["strategy"] = "default";
            result["budget_tokens"] = ::Json::Value{};
            result["note"] = "当前采用服务商默认行为";
            result["error"] = ::Json::Value{};
            auto draft = *input;
            draft["base_url"] = "http://127.0.0.1";
            if (!meowlive::protocol::json::is_unsigned_32((*input)["max_tokens"]))
            {
                send(invalid_model());
                return;
            }
            if (auto preview = meowlive::server::provider_config(draft))
            {
                preview->reasoning_provider = draft["provider"].asString();
                if (auto fields = meowlive::protocol::json::parse_object(
                        meowlive::adapters::provider_reasoning_preview(*preview));
                    fields && !fields->empty())
                {
                    const auto& native = static_cast<const ::Json::Value&>(*fields);
                    if (native["error"].isString())
                    {
                        result["error"] = native["error"];
                        result["strategy"] = "invalid";
                        result["note"] = "请调整推理档位或回答 token 上限";
                    }
                    else
                    {
                        result["strategy"] = "native";
                        result["note"] = "请求携带供应商推理参数";
                        result["effective"] = result["requested"];
                        const auto& generation = native["generationConfig"];
                        if (generation.isObject())
                        {
                            const auto& thinking = generation["thinkingConfig"];
                            if (thinking["thinkingBudget"].isUInt())
                            {
                                result["budget_tokens"] = thinking["thinkingBudget"];
                                result["strategy"] = "budget";
                            }
                            else if (thinking["thinkingLevel"].isString())
                            {
                                auto effective = thinking["thinkingLevel"].asString();
                                for (char& character : effective)
                                {
                                    if (character >= 'A' && character <= 'Z')
                                    {
                                        character += 'a' - 'A';
                                    }
                                }
                                result["effective"] = effective;
                            }
                        }
                        else if (native["reasoning"].isObject())
                        {
                            result["effective"] = native["reasoning"]["effort"];
                        }
                        else if (native["reasoning_effort"].isString())
                        {
                            result["effective"] = native["reasoning_effort"];
                        }
                        else if (native["output_config"].isObject())
                        {
                            result["effective"] = native["output_config"]["effort"];
                        }
                        result["provider_parameters"] = std::move(*fields);
                    }
                }
            }
            send(drogon::HttpResponse::newHttpJsonResponse(std::move(result)));
            return;
        }
        const bool list = path == "/api/llm/models";
        if (list)
        {
            ::Json::Value wrapped(::Json::objectValue);
            wrapped["api_key"] = (*input)["api_key"];
            wrapped["clear_api_key"] = input->get("clear_api_key", false);
            input->removeMember("api_key");
            input->removeMember("clear_api_key");
            if (!input->isMember("model"))
            {
                (*input)["model"] = "";
            }
            wrapped["settings"] = std::move(*input);
            input = std::move(wrapped);
        }
        auto candidate = profiles.model_candidate(*input, list);
        auto config = candidate ? meowlive::server::provider_config(*candidate) : std::nullopt;
        if (!config || (!list && config->model.empty()))
        {
            send(invalid_model());
            return;
        }
        config->reasoning_provider = candidate->get("provider", "custom").asString();
        meowlive::server::transport::submit_http_work(
            queue, list ? "/api/llm/models" : "/api/llm/test", send,
            [config = std::move(*config), list, stop]
            {
                if (stop.stop_requested())
                {
                    return meowlive::server::transport::server_stopping_response();
                }
                ::Json::Value result(::Json::objectValue);
                meowlive::adapters::ProviderModel model(config);
                if (list)
                {
                    const auto response = model.list_models(stop);
                    if (!response)
                    {
                        return stop.stop_requested()
                                   ? meowlive::server::transport::server_stopping_response()
                                   : provider_error(response.error().message);
                    }
                    result["base_url"] = config.base_url;
                    result["models"] = ::Json::Value(::Json::arrayValue);
                    for (const auto& id : *response)
                    {
                        ::Json::Value item(::Json::objectValue);
                        item["id"] = id;
                        item["name"] = id;
                        result["models"].append(std::move(item));
                    }
                }
                else
                {
                    const auto response = model.test_connection(stop);
                    if (!response)
                    {
                        return stop.stop_requested()
                                   ? meowlive::server::transport::server_stopping_response()
                                   : provider_error(response.error().message);
                    }
                    result["message"] = "模型连接测试通过";
                }
                return drogon::HttpResponse::newHttpJsonResponse(std::move(result));
            },
            "llm_busy", "模型测试队列繁忙", meowlive::platform::ExceptionDetails::Omit);
    }
}
