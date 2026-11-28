module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.graph_http;

import meowlive.standard;
import meowlive.application.work_queue;
import meowlive.server.agent_worker;
import meowlive.server.graph_worker;
import meowlive.server.viewer_admin;

export namespace meowlive::server::transport
{
    std::array<std::string_view, 3> graph_paths();
    std::string_view graph_route_methods(std::string_view path);
    void respond_graph(const drogon::HttpRequestPtr& request,
                       const std::shared_ptr<meowlive::server::GraphWorker>& graph,
                       const std::shared_ptr<meowlive::server::ViewerAdmin>& viewers,
                       const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                       meowlive::application::WorkQueue& work,
                       std::function<void(const drogon::HttpResponsePtr&)> send);
}
