module;

#include <toml++/toml.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <meowlive/msvc_stl.hpp>

module meowlive.server.configuration;

import meowlive.standard;

import meowlive.application.live_session;
import meowlive.server.graph_configuration;

namespace meowlive::server::config_detail
{
    void parse_listen_address(const std::string& listen, meowlive::server::ServerConfig& config)
    {
        const auto delimiter = listen.rfind(':');
        if (delimiter == std::string::npos)
        {
            invalid("server.listen_address");
        }
        auto address = listen.substr(0, delimiter);
        in_addr ipv4{};
        in6_addr ipv6{};
        bool valid_ip{};
        if (address.size() > 2 && address.front() == '[' && address.back() == ']')
        {
            address = address.substr(1, address.size() - 2);
            valid_ip = ::inet_pton(AF_INET6, address.c_str(), &ipv6) == 1;
        }
        else
        {
            valid_ip = ::inet_pton(AF_INET, address.c_str(), &ipv4) == 1;
        }
        unsigned short port{};
        const auto* end = listen.data() + listen.size();
        const auto parsed = std::from_chars(listen.data() + delimiter + 1, end, port);
        if (!valid_ip || parsed.ptr != end || parsed.ec != std::errc{} || port == 0)
        {
            invalid("server.listen_address");
        }
        config.address = std::move(address);
        config.port = port;
    }

    bool valid_origin(std::string_view origin)
    {
        if (!origin.starts_with("http://") && !origin.starts_with("https://"))
        {
            return false;
        }
        const auto authority = origin.substr(origin.find("://") + 3);
        return !authority.empty() && authority.find_first_of("/?#@") == std::string_view::npos &&
               std::ranges::all_of(authority,
                                   [](unsigned char character)
                                   {
                                       return character >= 0x21 && character <= 0x7e;
                                   });
    }

    void read_server(toml::node_view<const toml::node> section,
                     meowlive::server::ServerConfig& config)
    {
        std::string listen;
        if (section["listen_address"])
        {
            read_field(section, "listen_address", "server", listen);
            parse_listen_address(listen, config);
        }
        if (const auto origins = section["allowed_origins"])
        {
            const auto* array = origins.as_array();
            if (!array)
            {
                invalid("server.allowed_origins");
            }
            config.allowed_origins.clear();
            for (const auto& entry : *array)
            {
                const auto origin = entry.value_exact<std::string>();
                if (!origin || !valid_origin(*origin))
                {
                    invalid("server.allowed_origins");
                }
                config.allowed_origins.push_back(*origin);
            }
        }
        read_bounded_integer(section, "queue_capacity", "server", 64, config.queue_capacity);
        read_bounded_integer(section, "history_limit", "server", 1000, config.history_limit);
    }

    bool valid_env_name(std::string_view name)
    {
        for (std::size_t index = 0; index < name.size(); ++index)
        {
            const auto character = name[index];
            if (character != '_' && !(character >= 'A' && character <= 'Z') &&
                !(character >= 'a' && character <= 'z') &&
                !(index > 0 && character >= '0' && character <= '9'))
            {
                return false;
            }
        }
        return true;
    }

    void read_credential_source(toml::node_view<const toml::node> section, const std::string& role,
                                const std::filesystem::path& config_parent, bool enabled,
                                std::string& name, std::filesystem::path& file)
    {
        const auto env_key = role + "_token_env";
        const auto file_key = role + "_token_file";
        read_field(section, env_key, "auth", name);
        if (!valid_env_name(name))
        {
            invalid("auth." + env_key);
        }
        if (section[file_key])
        {
            std::string path;
            read_field(section, file_key, "auth", path);
            if (path.empty() || path.contains('\0'))
            {
                invalid("auth." + file_key);
            }
            file = config_parent / utf8_path(path);
        }
        if (enabled && name.empty() && file.empty())
        {
            invalid("auth." + env_key + ", auth." + file_key);
        }
    }
}
