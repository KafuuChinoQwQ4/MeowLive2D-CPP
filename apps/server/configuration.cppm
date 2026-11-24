module;

#include <toml++/toml.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#else
#include <arpa/inet.h>
#endif

export module meowlive.server.configuration;

#if !defined(_WIN32)
import std;
#endif

export namespace meowlive::server
{
    struct AuthConfig
    {
        bool enabled{};
        std::string admin_token_env{"MEOWLIVE_ADMIN_TOKEN"};
        std::string device_token_env{"MEOWLIVE_DEVICE_TOKEN"};
        std::filesystem::path admin_token_file;
        std::filesystem::path device_token_file;
        unsigned session_lifetime_seconds{28800};
    };

    struct ServerConfig
    {
        std::filesystem::path data_directory;
        std::string address{"127.0.0.1"};
        unsigned short port{19601};
        std::vector<std::string> allowed_origins{"http://127.0.0.1:1420", "http://localhost:1420",
                                                 "http://tauri.localhost"};
        unsigned queue_capacity{8};
        unsigned history_limit{50};
        meowlive::server::AuthConfig auth;
    };
}

namespace
{
    [[noreturn]] void invalid(const std::string& field)
    {
        throw std::runtime_error("配置无效 [" + field + "]");
    }

    template <typename Value>
    void read_field(toml::node_view<const toml::node> section, std::string_view key,
                    const std::string& section_name, Value& value)
    {
        const auto node = section[key];
        if (!node)
        {
            return;
        }
        const auto parsed = node.value_exact<Value>();
        if (!parsed)
        {
            invalid(section_name + "." + std::string(key));
        }
        value = *parsed;
    }

    std::filesystem::path utf8_path(const std::string& value)
    {
        return std::filesystem::path(std::u8string(value.begin(), value.end()));
    }

    std::filesystem::path default_data_directory()
    {
#if defined(_WIN32)
        if (const auto* directory = ::_wgetenv(L"MEOWLIVE_DATA_DIR"); directory && *directory)
        {
            return directory;
        }
        if (const auto* directory = ::_wgetenv(L"LOCALAPPDATA"); directory && *directory)
        {
            return std::filesystem::path(directory) / "MeowLive2D";
        }
        invalid("LOCALAPPDATA");
#else
        if (const auto* directory = std::getenv("MEOWLIVE_DATA_DIR"); directory && *directory)
        {
            return directory;
        }
        return "data";
#endif
    }

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

    void read_bounded_integer(toml::node_view<const toml::node> section, std::string_view key,
                              const std::string& section_name, unsigned maximum, unsigned& value)
    {
        std::int64_t parsed = value;
        read_field(section, key, section_name, parsed);
        if (parsed < 1 || parsed > maximum)
        {
            invalid(section_name + "." + std::string(key));
        }
        value = static_cast<unsigned>(parsed);
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

    void validate_keys(const toml::node_view<const toml::node> section, const std::string& name,
                       std::initializer_list<std::string_view> keys)
    {
        if (!section)
        {
            return;
        }
        const auto* table = section.as_table();
        if (!table)
        {
            invalid(name);
        }
        for (const auto& [key, value] : *table)
        {
            if (std::ranges::find(keys, key.str()) == keys.end())
            {
                // A malformed key can contain private text, so report its location.
                invalid(name + " 行 " + std::to_string(value.source().begin.line));
            }
        }
    }
}

export namespace meowlive::server
{
    meowlive::server::ServerConfig load_configuration(const std::filesystem::path& data_override,
                                                      const std::filesystem::path& config_override)
    {
        meowlive::server::ServerConfig config;
        config.data_directory = std::filesystem::absolute(
            data_override.empty() ? default_data_directory() : data_override);
        const auto path = config_override.empty() ? config.data_directory / "server.toml"
                                                  : std::filesystem::absolute(config_override);
        if (config_override.empty() && !std::filesystem::exists(path))
        {
            return config;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            invalid("server.toml");
        }

        try
        {
            const auto document = toml::parse(stream);
            const auto server = document["server"];
            const auto auth = document["auth"];
            validate_keys(server, "server",
                          {"listen_address", "queue_capacity", "history_limit", "allowed_origins"});
            validate_keys(auth, "auth",
                          {"enabled", "admin_token_env", "device_token_env", "admin_token_file",
                           "device_token_file", "session_lifetime_seconds"});
            read_server(server, config);
            read_field(auth, "enabled", "auth", config.auth.enabled);
            read_bounded_integer(auth, "session_lifetime_seconds", "auth", 28800,
                                 config.auth.session_lifetime_seconds);
            read_credential_source(auth, "admin", path.parent_path(), config.auth.enabled,
                                   config.auth.admin_token_env, config.auth.admin_token_file);
            read_credential_source(auth, "device", path.parent_path(), config.auth.enabled,
                                   config.auth.device_token_env, config.auth.device_token_file);
        }
        catch (const toml::parse_error& error)
        {
            invalid("server.toml 行 " + std::to_string(error.source().begin.line));
        }
        return config;
    }
}
