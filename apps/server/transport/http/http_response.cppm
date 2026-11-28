module;

#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.transport.http_response;

import meowlive.standard;

import meowlive.platform.exception_diagnostics;

export namespace meowlive::server::transport
{
    drogon::HttpResponsePtr error_response(drogon::HttpStatusCode status, std::string_view code,
                                           std::string_view message)
    {
        ::Json::Value content(::Json::objectValue);
        content["code"] = std::string(code);
        content["message"] = std::string(message);
        auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(content));
        response->setStatusCode(status);
        return response;
    }

    drogon::HttpResponsePtr internal_error_response(meowlive::platform::ExceptionKind failure)
    {
        if (failure == meowlive::platform::ExceptionKind::OutOfMemory)
        {
            return meowlive::server::transport::error_response(drogon::k500InternalServerError,
                                                               "out_of_memory", "服务内存分配失败");
        }
        return meowlive::server::transport::error_response(drogon::k500InternalServerError,
                                                           "internal_error", "服务内部错误");
    }
}
