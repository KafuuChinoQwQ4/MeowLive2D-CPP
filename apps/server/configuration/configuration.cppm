module;

#include <toml++/toml.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.configuration;

import meowlive.standard;

import meowlive.application.live_session;
import meowlive.server.graph_configuration;

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

    struct SpeechConfig
    {
        std::string base_url{"http://127.0.0.1:9880"};
        std::filesystem::path reference_audio;
        std::string prompt_text;
        std::string prompt_language{"zh"};
        std::string text_language{"zh"};
        unsigned timeout_seconds{300};
        unsigned max_audio_bytes{32 * 1024 * 1024};
        unsigned sentence_batch_size{4};
        std::filesystem::path resources_directory;
        std::filesystem::path receipt_directory;
    };

    struct LiveConfig
    {
        bool enabled{};
        std::uint64_t app_id{};
        std::string access_key_id_env{"MEOWLIVE_BILIBILI_ACCESS_KEY_ID"};
        std::string access_key_secret_env{"MEOWLIVE_BILIBILI_ACCESS_KEY_SECRET"};
        std::string identity_code_env{"MEOWLIVE_BILIBILI_IDENTITY_CODE"};
        meowlive::application::LiveRetry retry;
    };

    struct ViewerConfig
    {
        bool enabled{};
        bool agent_memory{};
        std::int64_t calendar_offset_minutes{480};
        std::string scope_id{"default-avatar"};
        std::string database_url_env{"MEOWLIVE_DATABASE_URL"};
        std::filesystem::path libpq;
        std::filesystem::path migrations;
    };

    struct MemoryConfig
    {
        bool enabled{};
        std::string endpoint;
        std::string model;
        std::string api_key_env{"MEOWLIVE_MEMORY_API_KEY"};
        std::string embedding_endpoint;
        std::string embedding_model;
        std::string embedding_api_key_env{"MEOWLIVE_EMBEDDING_API_KEY"};
        unsigned embedding_dimensions{384};
        unsigned timeout_ms{15000};
    };

    struct ServerConfig
    {
        std::filesystem::path data_directory;
        std::filesystem::path config_path;
        std::string address{"127.0.0.1"};
        unsigned short port{19601};
        std::vector<std::string> allowed_origins{"http://127.0.0.1:1420", "http://localhost:1420",
                                                 "http://tauri.localhost"};
        unsigned queue_capacity{8};
        unsigned history_limit{50};
        meowlive::server::AuthConfig auth;
        meowlive::server::SpeechConfig speech;
        meowlive::server::LiveConfig live;
        meowlive::server::ViewerConfig viewers;
        meowlive::server::MemoryConfig memory;
        meowlive::server::GraphConfig graph;
    };
}

namespace meowlive::server::config_detail
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

    void parse_listen_address(const std::string& listen, meowlive::server::ServerConfig& config);

    bool valid_origin(std::string_view origin);

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
                     meowlive::server::ServerConfig& config);

    bool valid_env_name(std::string_view name);

    void read_credential_source(toml::node_view<const toml::node> section, const std::string& role,
                                const std::filesystem::path& config_parent, bool enabled,
                                std::string& name, std::filesystem::path& file);

    void read_speech(const toml::table& document, meowlive::server::ServerConfig& config);

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
    void read_viewers(toml::node_view<const toml::node> section, const std::filesystem::path& base,
                      meowlive::server::ViewerConfig& viewers);

    void read_memory(toml::node_view<const toml::node> section, bool viewers_enabled,
                     meowlive::server::MemoryConfig& memory);

    void read_live(toml::node_view<const toml::node> section, meowlive::server::LiveConfig& live);

}

export namespace meowlive::server
{
    meowlive::server::ServerConfig load_configuration(const std::filesystem::path& data_override,
                                                      const std::filesystem::path& config_override)
    {
        meowlive::server::ServerConfig config;
        config.data_directory = std::filesystem::absolute(
            data_override.empty() ? meowlive::server::config_detail::default_data_directory()
                                  : data_override);
        config.config_path = config_override.empty() ? config.data_directory / "server.toml"
                                                     : std::filesystem::absolute(config_override);
        config.speech.resources_directory = config.data_directory / "resources";
        config.speech.receipt_directory = config.data_directory / "receipts";
        const auto& path = config.config_path;
        if (config_override.empty() && !std::filesystem::exists(path))
        {
            return config;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            meowlive::server::config_detail::invalid("server.toml");
        }

        try
        {
            const auto document = toml::parse(stream);
            const auto server = document["server"];
            const auto auth = document["auth"];
            meowlive::server::config_detail::validate_keys(
                server, "server",
                {"listen_address", "queue_capacity", "history_limit", "allowed_origins"});
            meowlive::server::config_detail::validate_keys(
                document["speech"], "speech",
                {"base_url", "reference_audio", "prompt_text", "prompt_language", "text_language",
                 "timeout_seconds", "max_audio_bytes", "sentence_batch_size", "max_concurrency"});
            meowlive::server::config_detail::validate_keys(
                auth, "auth",
                {"enabled", "admin_token_env", "device_token_env", "admin_token_file",
                 "device_token_file", "session_lifetime_seconds"});
            meowlive::server::config_detail::read_server(server, config);
            meowlive::server::config_detail::read_speech(document, config);
            meowlive::server::config_detail::read_live(document["live"], config.live);
            meowlive::server::config_detail::read_viewers(document["viewers"], path.parent_path(),
                                                          config.viewers);
            meowlive::server::config_detail::read_memory(document["memory"], config.viewers.enabled,
                                                         config.memory);
            config.graph = meowlive::server::read_graph(document["graph"], config.viewers.enabled);
            meowlive::server::config_detail::read_field(auth, "enabled", "auth",
                                                        config.auth.enabled);
            meowlive::server::config_detail::read_bounded_integer(
                auth, "session_lifetime_seconds", "auth", 28800,
                config.auth.session_lifetime_seconds);
            meowlive::server::config_detail::read_credential_source(
                auth, "admin", path.parent_path(), config.auth.enabled, config.auth.admin_token_env,
                config.auth.admin_token_file);
            meowlive::server::config_detail::read_credential_source(
                auth, "device", path.parent_path(), config.auth.enabled,
                config.auth.device_token_env, config.auth.device_token_file);
        }
        catch (const toml::parse_error& error)
        {
            meowlive::server::config_detail::invalid("server.toml 行 " +
                                                     std::to_string(error.source().begin.line));
        }
        return config;
    }
}
