module;

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <cstdio>

#if defined(_WIN32)
#include <exception>
#include <filesystem>
#include <memory>
#include <print>
#include <stdexcept>
#include <string_view>
#endif

export module meowlive.server.bootstrap;

#if !defined(_WIN32)
import std;
#endif

import meowlive.protocol.control;
import meowlive.server.configuration;
import meowlive.server.transport.access;
import meowlive.server.transport.http;
import meowlive.server.transport.websocket;

namespace
{
#if defined(_WIN32)
    using Argument = wchar_t;
#else
    using Argument = char;
#endif
    struct Paths
    {
        std::filesystem::path config;
        std::filesystem::path data;
    };

    Paths parse_arguments(int count, Argument* const* arguments)
    {
        Paths paths;
        for (int index = 1; index < count; ++index)
        {
            const std::filesystem::path option(arguments[index]);
            auto* destination = option == "--config"     ? &paths.config
                                : option == "--data-dir" ? &paths.data
                                                         : nullptr;
            if (!destination || !destination->empty() || ++index == count || !*arguments[index])
            {
                throw std::runtime_error("用法：meowlive-server [--config PATH] [--data-dir PATH]");
            }
            *destination = arguments[index];
        }
        return paths;
    }

    void request_shutdown()
    {
        auto& app = drogon::app();
        app.getLoop()->queueInLoop(
            []
            {
                LOG_INFO << "Stopping MeowLive2D server";
                meowlive::server::transport::shutdown_websocket();
                drogon::app().quit();
            });
    }

    void create_runtime_directory(const std::filesystem::path& path, std::string_view role)
    {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error)
        {
            throw std::runtime_error("无法创建 " + std::string(role));
        }
    }
}

export namespace meowlive::server
{
#if defined(_WIN32)
    int run(int count, wchar_t* const* arguments)
#else
    int run(int count, char* const* arguments)
#endif
    {
        try
        {
            const auto paths = parse_arguments(count, arguments);
            const auto config = meowlive::server::load_configuration(paths.data, paths.config);
            const auto logs = config.data_directory / "logs";
            const auto uploads = config.data_directory / "uploads";
            create_runtime_directory(logs, "data/logs");
            create_runtime_directory(uploads, "data/uploads");

            const auto access = std::make_shared<meowlive::server::transport::Access>(config);
            auto& app = drogon::app();
            app.setLogPath(drogon::utils::fromNativePath(logs.native()), "meowlive-server",
                           5 * 1024 * 1024, 4)
                .setLogLevel(trantor::Logger::kInfo)
                .setUploadPath(drogon::utils::fromNativePath(uploads.native()))
                .setFileTypes({})
                .setImplicitPageEnable(false)
                .setClientMaxBodySize(16 * 1024)
                .setClientMaxWebSocketMessageSize(64 * 1024)
                .setTermSignalHandler(request_shutdown)
                .setIntSignalHandler(request_shutdown)
                .addListener(config.address, config.port);
            app.registerBeginningAdvice(
                []
                {
                    LOG_INFO << "MeowLive2D server ready";
                });
            meowlive::server::transport::register_http(access);
            meowlive::server::transport::register_websocket(access);
            if (access->enabled() && !access->configured())
            {
                std::println(stderr, "认证已锁定，请检查 [auth] 凭据来源");
            }
            std::println("MeowLive2D protocol v{} ({}:{})", meowlive::protocol::version,
                         config.address, config.port);
            std::fflush(stdout);
            app.run();
            return 0;
        }
        catch (const std::filesystem::filesystem_error&)
        {
            std::println(stderr, "无法访问服务配置或数据目录");
        }
        catch (const std::runtime_error& error)
        {
            std::println(stderr, "{}", error.what());
        }
        catch (const std::exception&)
        {
            std::println(stderr, "服务启动失败");
        }
        return 1;
    }
}
