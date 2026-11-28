module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.memory_http;

import meowlive.standard;
import meowlive.adapters.storage.memory_admin;
import meowlive.application.work_queue;
import meowlive.server.agent_worker;

export namespace meowlive::server::transport
{
    std::string_view memory_route_methods(std::string_view path);
    void respond_memory(const drogon::HttpRequestPtr& request,
                        const std::shared_ptr<meowlive::adapters::storage::MemoryAdmin>& repository,
                        const std::shared_ptr<meowlive::server::AgentWorker>& agent,
                        meowlive::application::WorkQueue& work,
                        std::function<void(const drogon::HttpResponsePtr&)> send);
}
