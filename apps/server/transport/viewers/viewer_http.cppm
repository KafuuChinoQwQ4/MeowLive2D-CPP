module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.viewer_http;

import meowlive.standard;
import meowlive.application.work_queue;
import meowlive.server.viewer_admin;

export namespace meowlive::server::transport
{
    std::array<std::string_view, 6> viewer_paths();
    std::string_view viewer_route_methods(std::string_view path);
    void respond_viewers(const drogon::HttpRequestPtr& request,
                         const std::shared_ptr<meowlive::server::ViewerAdmin>& admin,
                         meowlive::application::WorkQueue& work,
                         std::function<void(const drogon::HttpResponsePtr&)> send,
                         std::function<void(bool)> knowledge_edit);
}
