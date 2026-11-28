module;

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <meowlive/msvc_stl.hpp>

module meowlive.server.transport.viewer_http;

import meowlive.standard;

import meowlive.application.work_queue;
import meowlive.adapters.storage.viewer_merge;
import meowlive.domain.speech;
import meowlive.protocol.json;
import meowlive.server.viewer_admin;
import meowlive.server.transport.access;
import meowlive.server.transport.media_type;
import meowlive.server.transport.storage_http;

namespace
{
    constexpr std::string_view profiles_path{"/api/admin/viewers"};
    constexpr std::string_view detail_path{"/api/admin/viewers/detail"};
    constexpr std::string_view viewer_status_path{"/api/admin/viewers/status"};
    constexpr std::string_view viewer_prefix{"/api/admin/viewers/"};
    constexpr std::string_view events_path{"/api/admin/events"};
    constexpr std::string_view preview_merge_path{"/api/admin/viewers/merge/preview"};
    constexpr std::string_view apply_merge_path{"/api/admin/viewers/merge/apply"};
    using Send = std::function<void(const drogon::HttpResponsePtr&)>;

    struct Page
    {
        unsigned limit{50};
        std::uint64_t offset{};
    };

    template <typename Number> bool number(std::string_view value, Number& output)
    {
        const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size();
    }

    std::optional<Page> page(const drogon::HttpRequestPtr& request)
    {
        Page result;
        for (const auto& [name, value] : request->getParameters())
        {
            if (!(name == "limit" && number(value, result.limit)) &&
                !(name == "offset" && number(value, result.offset)))
            {
                return {};
            }
        }
        if (result.limit < 1 || result.limit > 100 || result.offset > 1'000'000)
        {
            return {};
        }
        return result;
    }

    bool valid_id(std::string_view id)
    {
        if (id.size() != 36)
        {
            return false;
        }
        constexpr std::string_view hexadecimal{"0123456789abcdefABCDEF"};
        for (std::size_t index = 0; index < id.size(); ++index)
        {
            const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
            if (dash ? id[index] != '-' : !hexadecimal.contains(id[index]))
            {
                return false;
            }
        }
        return true;
    }

    std::string_view viewer_suffix(std::string_view path)
    {
        if (!path.starts_with(viewer_prefix))
        {
            return {};
        }
        path.remove_prefix(viewer_prefix.size());
        const auto uuid = path.substr(0, 36);
        return valid_id(uuid) ? uuid : std::string_view{};
    }

    struct AffinityOperation
    {
        std::string key;
        std::string reason;
        int delta{};
        std::string reversed;
    };

    std::optional<AffinityOperation> decode_affinity(const drogon::HttpRequestPtr& request,
                                                     bool reverse)
    {
        if (request->body().size() > 16 * 1024 ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return {};
        }
        const auto body = meowlive::protocol::json::parse_object(request->body());
        if (!body || body->size() != 3 || !(*body)["request_key"].isString() ||
            !valid_id((*body)["request_key"].asString()) || !(*body)["reason"].isString())
        {
            return {};
        }
        const auto reason = (*body)["reason"].asString();
        if (!meowlive::domain::normalize_speech_text(reason, 1000) ||
            reason.find_first_of("\t\r\n") != std::string::npos)
        {
            return {};
        }
        AffinityOperation operation{.key = (*body)["request_key"].asString(), .reason = reason};
        if (reverse)
        {
            if (!(*body)["ledger_id"].isString() || !valid_id((*body)["ledger_id"].asString()))
            {
                return {};
            }
            operation.reversed = (*body)["ledger_id"].asString();
        }
        else
        {
            const auto& delta = (*body)["delta_milli"];
            if (!delta.isInt() || delta.asInt() == 0 || delta.asInt() > 100'000 ||
                delta.asInt() < -100'000)
            {
                return {};
            }
            operation.delta = delta.asInt();
        }
        return operation;
    }

    drogon::HttpResponsePtr unavailable()
    {
        return meowlive::server::transport::error_response(
            drogon::k503ServiceUnavailable, "viewer_store_unavailable", "观众数据库暂不可用");
    }

    using MergeRequest = meowlive::adapters::storage::ViewerMergeRequest;

    std::optional<MergeRequest> decode_merge(const drogon::HttpRequestPtr& request, bool apply)
    {
        if (request->body().size() > 16 * 1024 ||
            !meowlive::server::transport::is_json_media_type(request->getHeader("content-type")))
        {
            return {};
        }
        const auto body = meowlive::protocol::json::parse_object(request->body());
        if (!body || body->size() != (apply ? 7u : 2u) || !(*body)["source_viewer_id"].isString() ||
            !(*body)["target_viewer_id"].isString())
        {
            return {};
        }
        MergeRequest options{.source = (*body)["source_viewer_id"].asString(),
                             .target = (*body)["target_viewer_id"].asString()};
        if (!valid_id(options.source) || !valid_id(options.target) ||
            options.source == options.target)
        {
            return {};
        }
        if (apply)
        {
            if (!(*body)["expected_revision"].isInt64() ||
                (*body)["expected_revision"].asInt64() < 0 || !(*body)["fingerprint"].isString() ||
                !(*body)["request_key"].isString() || !(*body)["reason"].isString() ||
                (*body)["confirmed"] != true)
            {
                return {};
            }
            options.revision = (*body)["expected_revision"].asInt64();
            options.fingerprint = (*body)["fingerprint"].asString();
            options.key = (*body)["request_key"].asString();
            options.reason = (*body)["reason"].asString();
            if (!valid_id(options.key) || options.fingerprint.size() != 64 ||
                options.reason.size() > 1000 ||
                !meowlive::domain::normalize_speech_text(options.reason, 1000))
            {
                return {};
            }
        }
        return options;
    }

    struct KnowledgeEdit
    {
        const std::function<void(bool)>& fence;
        explicit KnowledgeEdit(const std::function<void(bool)>& edit) : fence(edit)
        {
            fence(true);
        }
        ~KnowledgeEdit()
        {
            fence(false);
        }
    };

    drogon::HttpResponsePtr json(::Json::Value value)
    {
        auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(value));
        response->addHeader("cache-control", "no-store");
        return response;
    }
}

namespace meowlive::server::transport
{
    std::array<std::string_view, 6> viewer_paths()
    {
        return std::array{profiles_path, detail_path,        viewer_status_path,
                          events_path,   preview_merge_path, apply_merge_path};
    }

    std::string_view viewer_route_methods(std::string_view path)
    {
        if (path == preview_merge_path || path == apply_merge_path)
        {
            return "POST";
        }
        if (path == profiles_path || path == detail_path || path == viewer_status_path ||
            path == events_path)
        {
            return "GET,HEAD";
        }
        const auto id = viewer_suffix(path);
        if (id.empty())
        {
            return {};
        }
        const auto action = path.substr(viewer_prefix.size() + id.size());
        if (action.empty())
        {
            return "GET,HEAD";
        }
        return action == "/adjust" || action == "/reverse" ? "POST" : "";
    }

    void respond_viewers(const drogon::HttpRequestPtr& request,
                         const std::shared_ptr<meowlive::server::ViewerAdmin>& admin,
                         meowlive::application::WorkQueue& work, Send send,
                         std::function<void(bool)> knowledge_edit)
    {
        const std::string_view path = request->getOriginalPath();
        Page requested;
        const auto profile = viewer_suffix(path);
        std::string id(profile);
        std::optional<AffinityOperation> affinity_action;
        std::optional<MergeRequest> merge_action;
        if (path == preview_merge_path || path == apply_merge_path)
        {
            merge_action = decode_merge(request, path == apply_merge_path);
            if (!merge_action)
            {
                send(meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "invalid_viewer_merge",
                    "请核对身份、预览版本、合并理由和确认项"));
                return;
            }
        }
        else if (!profile.empty() && request->method() == drogon::Post)
        {
            affinity_action = decode_affinity(request, path.ends_with("/reverse"));
            if (!affinity_action)
            {
                send(meowlive::server::transport::error_response(drogon::k400BadRequest,
                                                                 "invalid_admin_operation",
                                                                 "观众、请求标识或调整依据无效"));
                return;
            }
        }
        if (path == detail_path)
        {
            const auto& fields = request->getParameters();
            const auto field = fields.find("viewer_id");
            if (fields.size() != 1 || field == fields.end() || !valid_id(field->second))
            {
                send(meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "invalid_viewer_id", "观众 ID 无效"));
                return;
            }
            id = field->second;
        }
        else if (path != viewer_status_path && profile.empty() && !merge_action)
        {
            const auto values = page(request);
            if (!values)
            {
                send(meowlive::server::transport::error_response(
                    drogon::k400BadRequest, "invalid_page", "分页超出有效范围"));
                return;
            }
            requested = *values;
        }

        auto operation =
            [admin, path = std::string(path), requested, id = std::move(id),
             profile_detail = !profile.empty(), affinity_action = std::move(affinity_action),
             merge_action = std::move(merge_action), knowledge_edit = std::move(knowledge_edit)]
        {
            if (path == viewer_status_path)
            {
                return json(admin->status());
            }
            if (!admin->enabled())
            {
                return unavailable();
            }
            if (merge_action)
            {
                std::optional<::Json::Value> result;
                if (path == apply_merge_path)
                {
                    const KnowledgeEdit edit(knowledge_edit);
                    result = admin->apply_merge(*merge_action);
                }
                else
                {
                    result = admin->preview_merge(merge_action->source, merge_action->target);
                }
                return result ? json(std::move(*result))
                              : meowlive::server::transport::error_response(
                                    drogon::k409Conflict, "viewer_merge_conflict",
                                    "观众资料已变化或合并冲突，请重新预览");
            }
            if (affinity_action)
            {
                const auto& action = *affinity_action;
                const auto record = admin->change_affinity(id, action.key, action.reason,
                                                           action.delta, action.reversed);
                if (!record)
                {
                    return meowlive::server::transport::error_response(
                        drogon::k409Conflict, "admin_operation_failed",
                        "操作未完成，请刷新资料后重试");
                }
                ::Json::Value result(::Json::objectValue);
                result["record_id"] = *record;
                return json(std::move(result));
            }
            if (path == detail_path || profile_detail)
            {
                auto detail = profile_detail ? admin->companionship(id) : admin->detail(id);
                return detail ? json(std::move(*detail))
                              : meowlive::server::transport::error_response(
                                    drogon::k404NotFound, "viewer_not_found", "观众档案不存在");
            }
            return json(path == events_path ? admin->events(requested.limit, requested.offset)
                                            : admin->list(requested.limit, requested.offset));
        };
        meowlive::server::transport::submit_storage_http(work, request->getMatchedPathPattern(),
                                                         send, std::move(operation), unavailable);
    }
}
