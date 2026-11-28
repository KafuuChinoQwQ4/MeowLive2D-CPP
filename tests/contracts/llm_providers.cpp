#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/Utilities.h>
#include <json/json.h>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.adapters.llm.language_model;
import meowlive.adapters.llm.memory_embedding;
import meowlive.adapters.llm.web_search;
import meowlive.application.llm;
import meowlive.application.memory_extraction;
import meowlive.protocol.json;

namespace
{
    namespace app = meowlive::application;
    namespace adapter = meowlive::adapters;

    constexpr std::string_view chat_tool =
        R"({"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"tool_1","type":"function","function":{"name":"status","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})";
    constexpr std::string_view responses_tool =
        R"({"status":"completed","output":[{"type":"reasoning","encrypted_content":"opaque"},{"type":"function_call","call_id":"tool_1","name":"status","arguments":"{}"}]})";
    constexpr std::string_view anthropic_tool =
        R"({"role":"assistant","stop_reason":"tool_use","content":[{"type":"tool_use","id":"tool_1","name":"status","input":{}}]})";
    constexpr std::string_view gemini_tool =
        R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"functionCall":{"name":"status","args":{}},"thoughtSignature":"opaque"}]}}]})";

    constexpr std::string_view chat_stream =
        "data: "
        R"({"choices":[{"index":0,"delta":{"content":"{\"reply_to\":[],"}}]})"
        "\r\n\r\n"
        "data: "
        R"({"choices":[{"index":0,"delta":{"content":"\"text\":\"完成\",\"topic\":null}"},"finish_reason":"stop"}]})"
        "\r\n\r\n"
        "data: "
        R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15,"prompt_tokens_details":{"cached_tokens":3,"cache_write_tokens":2},"completion_tokens_details":{"reasoning_tokens":1}}})"
        "\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    constexpr std::string_view responses_stream =
        "event: response.output_text.delta\ndata: "
        R"({"type":"response.output_text.delta","delta":"{"})"
        "\n\n"
        "event: response.completed\ndata: "
        R"({"type":"response.completed","response":{"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"{\"reply_to\":[],\"text\":\"完成\",\"topic\":null}"}]}],"usage":{"input_tokens":10,"output_tokens":5,"total_tokens":15,"input_tokens_details":{"cached_tokens":3,"cache_write_tokens":2},"output_tokens_details":{"reasoning_tokens":1}}}})"
        "\n\n";
    constexpr std::string_view anthropic_stream =
        "data: "
        R"({"type":"message_start","message":{"role":"assistant","content":[],"usage":{"input_tokens":10,"cache_read_input_tokens":3,"cache_creation_input_tokens":2}}})"
        "\n\n"
        "data: "
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"
        "\n\n"
        "data: "
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"{\"reply_to\":[],\"text\":\"完成\",\"topic\":null}"}})"
        "\n\n"
        "data: "
        R"({"type":"content_block_stop","index":0})"
        "\n\n"
        "data: "
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5,"output_tokens_details":{"thinking_tokens":1}}})"
        "\n\n"
        "data: "
        R"({"type":"message_stop"})"
        "\n\n";
    constexpr std::string_view gemini_stream =
        "data: "
        R"({"candidates":[{"content":{"parts":[{"text":"{\"reply_to\":[],","thought":false}]}}]})"
        "\n\n"
        "data: "
        R"({"candidates":[{"content":{"parts":[{"text":"\"text\":\"完成\",\"topic\":null}"}]},"finishReason":"STOP"}]})"
        "\n\n"
        "data: "
        R"({"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":5,"totalTokenCount":16,"cachedContentTokenCount":3,"thoughtsTokenCount":1}})"
        "\n\n";

    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    bool valid_round(const Json::Value& body, std::string_view kind, bool& continuation)
    {
        continuation = false;
        if (kind == "gemini")
        {
            const auto& parts = body["contents"];
            if (!parts.isArray() || parts.empty())
            {
                return false;
            }
            continuation = parts.size() == 3;
            if (continuation)
            {
                const auto& result = parts[2]["parts"][0]["functionResponse"];
                return !body.isMember("tools") &&
                       body["generationConfig"]["responseMimeType"] == "application/json" &&
                       parts[1]["parts"][0]["thoughtSignature"] == "opaque" &&
                       result["name"] == "status" && !result.isMember("id") &&
                       result["response"]["result"]["connected"] == true;
            }
            return body["tools"][0]["functionDeclarations"][0]["name"] == "status" &&
                   !body["generationConfig"].isMember("responseMimeType");
        }
        const auto& messages = body[kind == "responses" ? "input" : "messages"];
        if (!messages.isArray() || messages.empty() || !body["stream"].asBool())
        {
            return false;
        }
        if (kind == "anthropic")
        {
            continuation = messages.size() > 1;
            if (continuation)
            {
                return body["tool_choice"]["type"] == "none" &&
                       messages[messages.size() - 1]["content"][0]["tool_use_id"] == "tool_1";
            }
            return body["tools"][0]["name"] == "status";
        }
        continuation = messages.size() > 2;
        if (kind == "responses")
        {
            if (continuation)
            {
                return body["tool_choice"] == "none" &&
                       messages[2]["encrypted_content"] == "opaque" &&
                       messages[messages.size() - 1]["call_id"] == "tool_1";
            }
            return body["tools"][0]["name"] == "status" && body["store"] == false;
        }
        if (continuation)
        {
            return body["tool_choice"] == "none" &&
                   messages[messages.size() - 1]["tool_call_id"] == "tool_1";
        }
        return body["tools"][0]["function"]["name"] == "status";
    }

    bool valid_memory_request(const Json::Value& body)
    {
        const auto& messages = body["messages"];
        if (!messages.isArray() || messages.size() != 2 || body["stream"] == true ||
            body.isMember("tools") || body["response_format"]["type"] != "json_object" ||
            messages[0]["role"] != "system" || messages[1]["role"] != "user" ||
            !messages[0]["content"].asString().contains("untrusted data"))
        {
            return false;
        }
        const auto sources =
            meowlive::protocol::json::parse_container(messages[1]["content"].asString());
        return sources && sources->isArray() && sources->size() == 1 && (*sources)[0].size() == 3 &&
               (*sources)[0]["event_id"] == "evt-1" && (*sources)[0]["source"] == "bilibili" &&
               (*sources)[0]["text"] == "请叫我月亮";
    }

    void embedding_reply(const drogon::HttpRequestPtr& request, const Json::Value& body,
                         const drogon::HttpResponsePtr& response, bool authenticated)
    {
        const auto& input = body["input"];
        if (request->getHeader("authorization") !=
                (authenticated ? "Bearer provider-fixture" : "") ||
            body["model"] != "fixture" || body["dimensions"] != 2 ||
            body["encoding_format"] != "float" || !input.isArray() || input.size() != 1 ||
            !input[0].isString())
        {
            return;
        }
        const auto text = input[0].asString();
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString("application/json");
        if (text == "short-vector")
        {
            response->setBody(R"({"model":"fixture","data":[{"index":0,"embedding":[0.5]}]})");
        }
        else if (text == "float-overflow")
        {
            response->setBody(
                R"({"model":"fixture","data":[{"index":0,"embedding":[0.5,1e100]}]})");
        }
        else if (text == "unexpected-model")
        {
            response->setBody(
                R"({"model":"different","data":[{"index":0,"embedding":[0.5,-0.25]}]})");
        }
        else if (text == "unexpected-index")
        {
            response->setBody(
                R"({"model":"fixture","data":[{"index":1,"embedding":[0.5,-0.25]}]})");
        }
        else if (text == "oversized-response")
        {
            response->setBody(std::string(65'537, ' '));
        }
        else if (text == "跑步")
        {
            response->setBody(
                R"({"model":"fixture","data":[{"index":0,"embedding":[0.5,-0.25]}]})");
        }
        else
        {
            response->setStatusCode(drogon::k400BadRequest);
        }
    }

    drogon::HttpResponsePtr reply(const drogon::HttpRequestPtr& request)
    {
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k400BadRequest);
        const auto path = request->path();
        const auto slash = path.find('/', 1);
        if (slash == std::string::npos)
        {
            return response;
        }
        const auto provider = std::string_view(path).substr(1, slash - 1);
        auto endpoint = std::string_view(path).substr(slash);
        if (!endpoint.starts_with("/v1/"))
        {
            return response;
        }
        endpoint.remove_prefix(3);
        if (provider == "status")
        {
            response->setStatusCode(drogon::k429TooManyRequests);
            return response;
        }
        if (provider == "search" && request->method() == drogon::Get &&
            request->getParameter("q") == "猫 & x=y")
        {
            const bool brave = endpoint == "/brave" && request->getParameter("count") == "5" &&
                               request->getHeader("x-subscription-token") == "provider-fixture";
            const bool searxng = endpoint == "/searxng" &&
                                 request->getParameter("format") == "json" &&
                                 request->getHeader("x-subscription-token").empty();
            if (brave || searxng)
            {
                response->setStatusCode(drogon::k200OK);
                response->setContentTypeString("application/json");
                response->setBody(
                    brave
                        ? R"({"web":{"results":[{"title":"Cat","url":"https://example.com","description":"Snippet"}]}})"
                        : R"({"results":[{"title":"Cat","url":"https://example.com","content":"Snippet"}]})");
            }
            return response;
        }
        if ((provider == "gemini" && request->getHeader("x-goog-api-key") != "provider-fixture") ||
            (provider == "anthropic" &&
             (request->getHeader("x-api-key") != "provider-fixture" ||
              request->getHeader("anthropic-version") != "2023-06-01")) ||
            ((provider == "chat" || provider == "responses" || provider == "memory") &&
             request->getHeader("authorization") != "Bearer provider-fixture"))
        {
            return response;
        }
        if (endpoint == "/models" && request->method() == drogon::Get)
        {
            response->setStatusCode(drogon::k200OK);
            response->setContentTypeString("application/json");
            response->setBody(
                provider == "gemini"
                    ? R"({"models":[{"name":"models/fixture","supportedGenerationMethods":["generateContent"]}]})"
                    : R"({"data":[{"id":"fixture"}]})");
            return response;
        }
        if (request->method() != drogon::Post ||
            (provider == "chat" && endpoint != "/chat/completions") ||
            (provider == "responses" && endpoint != "/responses") ||
            (provider == "anthropic" && endpoint != "/messages") ||
            (provider == "gemini" && (endpoint != "/models/fixture:streamGenerateContent" ||
                                      request->getParameter("alt") != "sse")))
        {
            return response;
        }
        const auto payload = meowlive::protocol::json::parse_object(request->body());
        if ((provider == "embed" || provider == "embed-public") && endpoint == "/embeddings" &&
            payload)
        {
            embedding_reply(request, *payload, response, provider == "embed");
            return response;
        }
        if (provider == "memory" && endpoint == "/chat/completions" && payload &&
            valid_memory_request(*payload))
        {
            response->setStatusCode(drogon::k200OK);
            response->setContentTypeString("application/json");
            response->setBody(
                R"({"choices":[{"message":{"role":"assistant","content":"{\"candidates\":[{\"key\":\"preferred_name\",\"value\":\"月亮\",\"kind\":\"preferred_name\",\"evidence\":[{\"source\":\"bilibili\",\"event_id\":\"evt-1\",\"quote\":\"请叫我月亮\"}],\"explicit\":true,\"confidence\":0.8,\"valid_until_ms\":null}]}"},"finish_reason":"stop"}]})");
            return response;
        }
        bool continuation{};
        if (!payload || !valid_round(*payload, provider, continuation))
        {
            return response;
        }
        const auto body =
            provider == "chat"        ? (continuation ? chat_stream : chat_tool)
            : provider == "responses" ? (continuation ? responses_stream : responses_tool)
            : provider == "anthropic" ? (continuation ? anthropic_stream : anthropic_tool)
            : continuation            ? gemini_stream
                                      : gemini_tool;
        response->setStatusCode(drogon::k200OK);
        response->setContentTypeString(continuation ? "text/event-stream" : "application/json");
        response->setBody(std::string(body));
        return response;
    }

    class Server
    {
    public:
        Server()
        {
            auto ready = std::make_shared<std::promise<std::uint16_t>>();
            auto port = ready->get_future();
            auto& server = drogon::app();
            server.disableSigtermHandling()
                .setThreadNum(1)
                .enableGzip(false)
                .setLogLevel(trantor::Logger::kError)
                .setUploadPath(
                    drogon::utils::fromNativePath(std::filesystem::temp_directory_path().native()))
                .addListener("127.0.0.1", 0);
            server.registerHandlerViaRegex(
                "/.*",
                [](const drogon::HttpRequestPtr& request,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback)
                {
                    callback(reply(request));
                },
                {drogon::Get, drogon::Post});
            server.registerBeginningAdvice(
                [ready]
                {
                    const auto listeners = drogon::app().getListeners();
                    ready->set_value(listeners.empty() ? 0 : listeners.front().toPort());
                });
            thread_ = std::thread(
                [ready]
                {
                    try
                    {
                        drogon::app().run();
                    }
                    catch (...)
                    {
                        try
                        {
                            ready->set_exception(std::current_exception());
                        }
                        catch (const std::future_error&)
                        {
                        }
                    }
                });
            if (port.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
            {
                try
                {
                    port_ = port.get();
                }
                catch (...)
                {
                    drogon::app().quit();
                    thread_.join();
                    throw;
                }
            }
            if (!port_)
            {
                drogon::app().quit();
                thread_.join();
                throw std::runtime_error("Provider fixture listener is unavailable");
            }
        }

        ~Server()
        {
            drogon::app().quit();
            thread_.join();
        }

        std::string url(std::string_view kind) const
        {
            return "http://127.0.0.1:" + std::to_string(port_) + "/" + std::string(kind) + "/v1";
        }

    private:
        std::uint16_t port_{};
        std::thread thread_;
    };

    void embeddings(const Server& server)
    {
        adapter::EmbeddingConfig config{server.url("embed") + "/", "fixture", "provider-fixture",
                                        2};
        constexpr auto timeout = std::chrono::seconds(2);
        const auto vector = adapter::embed_memory(config, "跑步", timeout, {});
        require(vector.has_value(), vector ? "embedding response" : vector.error().message);
        require(*vector == "[0.5,-0.25]", "pgvector serialization mismatch");
        for (const auto* text : {"short-vector", "float-overflow", "unexpected-model",
                                 "unexpected-index", "oversized-response"})
        {
            const auto invalid = adapter::embed_memory(config, text, timeout, {});
            require(!invalid &&
                        invalid.error().message == (std::string_view(text) == "oversized-response"
                                                        ? "模型响应超过大小上限"
                                                        : "记忆嵌入模型响应无效"),
                    "invalid embedding was not rejected by the adapter");
        }
        std::stop_source stopped;
        stopped.request_stop();
        require(!adapter::embed_memory(config, "跑步", timeout, stopped.get_token()),
                "cancelled embedding returned data");
        require(!adapter::embed_memory(config, " \u3000", timeout, {}),
                "blank embedding input was accepted");
        auto unsafe = config;
        unsafe.api_key += "\r\ninvalid-header";
        require(!adapter::embed_memory(unsafe, "跑步", timeout, {}),
                "invalid embedding key was accepted");
        config.endpoint = server.url("embed-public") + "/embeddings";
        config.api_key.clear();
        const auto public_vector = adapter::embed_memory(config, "跑步", timeout, {});
        require(public_vector && *public_vector == *vector,
                "explicit unauthenticated embedding endpoint failed");
        config.endpoint = server.url("status");
        const auto limited = adapter::embed_memory(config, "跑步", timeout, {});
        require(!limited && limited.error().retryable, "embedding HTTP 429 is retryable");
    }

    void providers()
    {
        using enum adapter::ProviderFormat;
        const Server server;
        embeddings(server);
        const std::array cases{
            std::pair{OpenaiChat, "chat"}, std::pair{OpenaiResponses, "responses"},
            std::pair{AnthropicMessages, "anthropic"}, std::pair{GeminiGenerateContent, "gemini"}};
        for (const auto& [format, name] : cases)
        {
            adapter::ProviderModel provider({.format = format,
                                             .base_url = server.url(name),
                                             .model = "fixture",
                                             .api_key = "provider-fixture"});
            const auto models = provider.list_models();
            require(models.has_value(), models ? "model listing" : models.error().message);
            require(models && *models == std::vector<std::string>{"fixture"},
                    "model listing failed");
            const app::DecisionRequest request{.persona = "主播", .system_prompt = "选择下一条。"};
            app::ModelOptions options{.tools = {{"status", "Status", "{\"type\":\"object\"}"}}};
            const auto first = provider.turn(request, options, {});
            require(first.has_value(), first ? "first turn" : first.error().message);
            require(first->continuation && first->tool_calls.size() == 1 &&
                        first->tool_calls.front().name == "status" &&
                        first->tool_calls.front().arguments_json == "{}",
                    "tool call parse failed");
            options.continuation = first->continuation;
            options.results.push_back(
                {first->tool_calls.front().id, "status", "{\"connected\":true}"});
            options.final_turn = true;
            const auto final = provider.turn(request, options, {});
            require(final.has_value(), final ? "final turn" : final.error().message);
            const auto expected_input = format == AnthropicMessages ? 15U : 10U;
            const auto expected_output = format == GeminiGenerateContent ? 6U : 5U;
            require(final->decision && final->decision->text == "完成" &&
                        final->tool_calls.empty() && final->usage.input == expected_input &&
                        final->usage.output == expected_output &&
                        final->usage.total == expected_input + expected_output,
                    "final streaming turn or usage failed");
            require(final->usage.cache_read == 3 && final->usage.reasoning == 1 &&
                        final->first_response_ms.has_value(),
                    "cache, reasoning or first body timing missing");
            require(format == GeminiGenerateContent ? !final->usage.cache_write
                                                    : final->usage.cache_write == 2,
                    "provider cache write usage mismatch");
        }
        adapter::ProviderModel limited({.base_url = server.url("status"), .model = "fixture"});
        const auto result = limited.turn({}, {}, {});
        require(!result && result.error().retryable, "HTTP 429 is retryable");
        adapter::ProviderModel memory_model(
            {.base_url = server.url("memory"), .model = "fixture", .api_key = "provider-fixture"});
        const std::array sources{
            app::MemorySource{"bilibili", "viewer", "evt-1", "请叫我月亮", 100, 0}};
        const auto memories = app::extract_memories(memory_model, sources, 101, {});
        require(memories.has_value(), memories ? "memory response" : memories.error().message);
        require(memories->size() == 1 && memories->front().status == "long_term" &&
                    memories->front().value == "月亮",
                "provider memory extraction response mismatch");
        for (const auto* name : {"brave", "searxng"})
        {
            const auto response =
                adapter::search_web({.provider = name,
                                     .base_url = server.url("search") + "/" + name,
                                     .api_key = "provider-fixture"},
                                    "猫 & x=y");
            require(response.has_value(), response ? "search" : response.error().message);
            const auto payload = meowlive::protocol::json::parse_object(*response);
            require(payload && (*payload)["results"].size() == 1 &&
                        (*payload)["results"][0]["snippet"] == "Snippet",
                    "search result format");
        }
        require(!adapter::search_web(
                    {.provider = "searxng", .base_url = "http://example.com/search"}, "cat"),
                "search rejects non-loopback HTTP");
        const auto reasoning = meowlive::protocol::json::parse_object(
            adapter::provider_reasoning_preview({.format = GeminiGenerateContent,
                                                 .model = "gemini-3-pro-preview",
                                                 .reasoning_effort = "max",
                                                 .reasoning_provider = "gemini"}));
        require(reasoning &&
                    (*reasoning)["generationConfig"]["thinkingConfig"]["thinkingLevel"] == "HIGH",
                "reasoning preview clamps to provider capability");
    }
}

int main()
{
    try
    {
        providers();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
