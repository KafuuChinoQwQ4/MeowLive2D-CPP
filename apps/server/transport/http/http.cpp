module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.http;

import meowlive.standard;

import meowlive.adapters.storage.memory_admin;
import meowlive.application.speech_worker;
import meowlive.application.work_queue;
import meowlive.platform.exception_diagnostics;
import meowlive.protocol.control;
import meowlive.protocol.json;
import meowlive.server.agent_worker;
import meowlive.server.graph_worker;
import meowlive.server.live;
import meowlive.server.profile_settings;
import meowlive.server.speech_voices;
import meowlive.server.training_worker;
import meowlive.server.viewer_admin;
import meowlive.server.transport.access;
import meowlive.server.transport.agent_http;
import meowlive.server.transport.graph_http;
import meowlive.server.transport.memory_http;
import meowlive.server.transport.viewer_http;
import meowlive.server.transport.http_work;
import meowlive.server.transport.character_http;
import meowlive.server.transport.live_http;
import meowlive.server.transport.media_type;
import meowlive.server.transport.model_http;
import meowlive.server.transport.profile_http;
import meowlive.server.transport.speech_http;
import meowlive.server.transport.voice_http;
import meowlive.server.transport.training_http;

namespace meowlive::server::transport
{
    void
    bind_http_routes(const std::shared_ptr<meowlive::server::transport::Access>& access,
                     std::span<const std::string_view> paths,
                     const std::function<void(const drogon::HttpRequestPtr&,
                                              meowlive::server::transport::HttpSend&&)>& handler);
}

namespace
{
    constexpr std::size_t login_body_limit = 16 * 1024;
}

namespace meowlive::server::transport
{
    std::string_view http_operation(const drogon::HttpRequestPtr& request)
    {
        const auto route = request->getMatchedPathPattern();
        return route.empty() ? "HTTP request" : route;
    }
    std::string_view route_methods(std::string_view path)
    {
        if (path == meowlive::server::transport::session_path)
        {
            return "GET,HEAD,POST,DELETE";
        }
        if (path == meowlive::protocol::health_path ||
            path == meowlive::server::transport::speech_status_path)
        {
            return "GET,HEAD";
        }
        if (path == meowlive::server::transport::speech_path ||
            path == meowlive::server::transport::preview_path ||
            path == meowlive::server::transport::stop_path)
        {
            return "POST";
        }
        if (auto methods = meowlive::server::transport::agent_route_methods(path); !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::model_route_methods(path); !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::live_route_methods(path); !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::voice_route_methods(path); !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::training_route_methods(path);
            !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::character_route_methods(path);
            !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::viewer_route_methods(path);
            !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::graph_route_methods(path); !methods.empty())
        {
            return methods;
        }
        if (auto methods = meowlive::server::transport::memory_route_methods(path);
            !methods.empty())
        {
            return methods;
        }
        return meowlive::server::transport::profile_route_methods(path);
    }

    bool is_large_http_upload(std::string_view path, bool post)
    {
        return post && (meowlive::server::transport::is_voice_upload(path, drogon::Post) ||
                        meowlive::server::transport::is_training_upload(path, drogon::Post));
    }
}

namespace
{
    drogon::HttpResponsePtr health(const meowlive::application::SpeechWorker& speech)
    {
        ::Json::Value content(::Json::objectValue);
        content["service"] = "meowlive";
        content["protocol_version"] = meowlive::protocol::version;
        content["bridge_connected"] = speech.connected();
        return drogon::HttpResponse::newHttpJsonResponse(std::move(content));
    }

    drogon::HttpResponsePtr invalid_login()
    {
        return meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                           "invalid_request", "需要 token 字符串");
    }

    drogon::HttpResponsePtr login(const drogon::HttpRequestPtr& request,
                                  meowlive::server::transport::Access& access)
    {
        if (request->body().size() > login_body_limit ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return invalid_login();
        }

        const auto content = meowlive::protocol::json::parse_container(request->body());
        if (!content || content->size() != 1)
        {
            return invalid_login();
        }
        // Serde also accepts a single positional field for this request struct.
        const auto& credential = content->isObject() ? (*content)["token"] : (*content)[0u];
        if (!credential.isString())
        {
            return invalid_login();
        }

        const auto session = access.login(credential.asString());
        using Result = meowlive::server::transport::LoginResult;
        switch (session.result)
        {
        case Result::Disabled:
            return meowlive::server::transport::error_response(drogon::k403Forbidden,
                                                               "auth_disabled", "管理员认证未启用");
        case Result::Invalid:
            return meowlive::server::transport::authentication_required();
        case Result::RateLimited:
            return meowlive::server::transport::error_response(
                drogon::k429TooManyRequests, "login_rate_limited", "登录失败次数过多，请稍后重试");
        case Result::Success:
            ::Json::Value answer(::Json::objectValue);
            answer["token"] = session.token;
            answer["expires_in_seconds"] = session.expires_in_seconds;
            auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(answer));
            response->addHeader("cache-control", "no-store");
            return response;
        }
        return invalid_login();
    }

    drogon::HttpResponsePtr admin_session(const drogon::HttpRequestPtr& request,
                                          meowlive::server::transport::Access& access)
    {
        switch (request->method())
        {
        case drogon::Get:
        {
            ::Json::Value answer(::Json::objectValue);
            answer["enabled"] = access.enabled();
            answer["authenticated"] =
                !access.enabled() ||
                access.is_admin_session(meowlive::server::transport::bearer(request));
            auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(answer));
            response->addHeader("cache-control", "no-store");
            return response;
        }
        case drogon::Post:
            return login(request, access);
        case drogon::Delete:
            if (access.revoke(meowlive::server::transport::bearer(request)))
            {
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(drogon::k204NoContent);
                response->addHeader("cache-control", "no-store");
                return response;
            }
            return meowlive::server::transport::authentication_required();
        default:
            return {};
        }
    }

    drogon::HttpResponsePtr reject_request(const drogon::HttpRequestPtr& request,
                                           meowlive::server::transport::Access& access)
    {
        const std::string_view path = request->getOriginalPath();
        const auto methods = meowlive::server::transport::route_methods(path);
        if (methods.empty())
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k404NotFound);
            return response;
        }
        if (auto rejected = meowlive::server::transport::origin_rejection(request, access))
        {
            return rejected;
        }
        const auto method = request->method();
        const bool get = method == drogon::Get && !request->isHead();
        const bool public_route =
            (path == meowlive::protocol::health_path && get) ||
            (path == meowlive::server::transport::session_path && (get || method == drogon::Post));
        const bool supported = (method == drogon::Get && methods.starts_with("GET")) ||
                               (method == drogon::Post && methods.contains("POST")) ||
                               (method == drogon::Delete && methods.contains("DELETE"));
        if (access.enabled() && !public_route &&
            !access.is_admin_session(meowlive::server::transport::bearer(request)))
        {
            auto response = meowlive::server::transport::authentication_required();
            if (!supported)
            {
                response->addHeader("allow", std::string(methods));
            }
            return response;
        }
        if (!supported)
        {
            auto response = drogon::HttpResponse::newHttpResponse();
            response->setStatusCode(drogon::k405MethodNotAllowed);
            response->addHeader("allow", std::string(methods));
            return response;
        }
        return {};
    }

    drogon::HttpResponsePtr respond(const drogon::HttpRequestPtr& request,
                                    meowlive::server::transport::Access& access,
                                    meowlive::application::SpeechWorker& speech,
                                    meowlive::server::AgentWorker& agent)
    {
        const std::string_view path = request->getOriginalPath();
        if (!meowlive::server::transport::agent_route_methods(path).empty())
        {
            return meowlive::server::transport::respond_agent(request, agent);
        }
        if (path == meowlive::protocol::health_path)
        {
            return health(speech);
        }
        if (path == meowlive::server::transport::session_path)
        {
            return admin_session(request, access);
        }
        if (path == meowlive::server::transport::speech_path)
        {
            return meowlive::server::transport::enqueue_speech(request, speech);
        }
        if (path == meowlive::server::transport::preview_path)
        {
            return meowlive::server::transport::enqueue_preview(speech);
        }
        return meowlive::server::transport::speech_status(
            path == meowlive::server::transport::stop_path ? agent.stop() : speech.status());
    }
}

namespace meowlive::server::transport
{
    void register_http(const std::shared_ptr<meowlive::server::transport::Access>& access,
                       const std::shared_ptr<meowlive::application::SpeechWorker>& speech,
                       const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                       const std::shared_ptr<meowlive::server::ProfileSettings>& profiles,
                       const std::shared_ptr<meowlive::server::transport::HttpWorkQueues>& queues,
                       const std::shared_ptr<meowlive::server::LiveService>& live,
                       const std::shared_ptr<meowlive::server::VoiceLibrary>& voices,
                       const std::shared_ptr<meowlive::server::TrainingWorker>& training,
                       const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                       const std::shared_ptr<meowlive::adapters::storage::MemoryAdmin>& memory,
                       const std::shared_ptr<meowlive::server::GraphWorker>& graph)
    {
        const auto dispatch = [access, speech, agent, profiles, live, voices, training, viewers,
                               memory, graph, queues](const drogon::HttpRequestPtr& request,
                                                      meowlive::server::transport::HttpSend send)
        {
            if (const auto rejection = reject_request(request, *access))
            {
                send(rejection);
                return;
            }
            const auto stop = queues->stop_token();
            if (stop.stop_requested())
            {
                send(meowlive::server::transport::server_stopping_response());
                return;
            }
            auto& work = queues->control;
            const auto& path = request->getOriginalPath();
            if (!meowlive::server::transport::graph_route_methods(path).empty())
            {
                meowlive::server::transport::respond_graph(request, graph, viewers, agent, work,
                                                           std::move(send));
                return;
            }
            if (!meowlive::server::transport::memory_route_methods(path).empty())
            {
                meowlive::server::transport::respond_memory(request, memory, agent, work,
                                                            std::move(send));
                return;
            }
            if (!meowlive::server::transport::viewer_route_methods(path).empty())
            {
                meowlive::server::transport::respond_viewers(request, viewers, work,
                                                             std::move(send),
                                                             [agent](bool begin)
                                                             {
                                                                 if (begin)
                                                                 {
                                                                     agent->begin_knowledge_edit();
                                                                 }
                                                                 else
                                                                 {
                                                                     agent->end_knowledge_edit();
                                                                 }
                                                             });
                return;
            }
            if (!meowlive::server::transport::training_route_methods(path).empty())
            {
                meowlive::server::transport::respond_training(request, training, speech, voices,
                                                              work, queues->auditions, stop,
                                                              std::move(send));
                return;
            }
            if (!meowlive::server::transport::voice_route_methods(path).empty())
            {
                meowlive::server::transport::respond_voice(request, voices, speech, work,
                                                           std::move(send));
                return;
            }
            if (!meowlive::server::transport::character_route_methods(path).empty())
            {
                meowlive::server::transport::respond_character(request, voices, speech, work,
                                                               std::move(send));
                return;
            }
            if (!meowlive::server::transport::live_route_methods(path).empty())
            {
                meowlive::server::transport::respond_live(request, live, work, std::move(send));
                return;
            }
            if (path == "/api/agent/runtime")
            {
                meowlive::server::transport::respond_runtime(request, agent->runtime_settings(),
                                                             work, std::move(send));
                return;
            }
            if (!meowlive::server::transport::profile_route_methods(path).empty())
            {
                meowlive::server::transport::respond_profile(request, *profiles, std::move(send),
                                                             [agent]
                                                             {
                                                                 return agent->snapshot();
                                                             });
                return;
            }
            if (!meowlive::server::transport::model_route_methods(path).empty())
            {
                meowlive::server::transport::respond_model(request, *profiles, queues->model_probes,
                                                           stop, std::move(send));
                return;
            }
            send(respond(request, *access, *speech, *agent));
        };

        const auto handler = [dispatch](const drogon::HttpRequestPtr& request,
                                        meowlive::server::transport::HttpSend&& send)
        {
            const auto operation = meowlive::server::transport::http_operation(request);
            try
            {
                dispatch(request,
                         [operation, send](const drogon::HttpResponsePtr& response) noexcept
                         {
                             meowlive::server::transport::deliver_http_response(operation, send,
                                                                                response);
                         });
            }
            catch (...)
            {
                meowlive::server::transport::send_http_exception(
                    operation, send, meowlive::platform::ExceptionDetails::Omit);
            }
        };
        std::vector<std::string_view> paths{meowlive::protocol::health_path,
                                            meowlive::server::transport::speech_status_path,
                                            meowlive::server::transport::session_path,
                                            meowlive::server::transport::speech_path,
                                            meowlive::server::transport::preview_path,
                                            meowlive::server::transport::stop_path};
        const auto append = [&paths](const auto& group)
        {
            paths.insert(paths.end(), group.begin(), group.end());
        };
        append(meowlive::server::transport::agent_paths());
        append(meowlive::server::transport::profile_paths());
        append(meowlive::server::transport::model_paths());
        append(meowlive::server::transport::live_paths());
        append(meowlive::server::transport::voice_paths());
        append(meowlive::server::transport::training_paths());
        append(meowlive::server::transport::viewer_paths());
        append(meowlive::server::transport::graph_paths());
        append(meowlive::server::transport::character_paths());
        meowlive::server::transport::bind_http_routes(access, paths, handler);
    }
}
