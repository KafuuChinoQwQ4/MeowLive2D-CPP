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
    void read_speech(const toml::table& document, meowlive::server::ServerConfig& config)
    {
        const auto section = document["speech"];
        auto& speech = config.speech;
        read_field(section, "base_url", "speech", speech.base_url);
        read_field(section, "prompt_text", "speech", speech.prompt_text);
        read_field(section, "prompt_language", "speech", speech.prompt_language);
        read_field(section, "text_language", "speech", speech.text_language);
        read_bounded_integer(section, "timeout_seconds", "speech", 300, speech.timeout_seconds);
        read_bounded_integer(section, "max_audio_bytes", "speech", 32 * 1024 * 1024,
                             speech.max_audio_bytes);
        read_bounded_integer(section, "sentence_batch_size", "speech", 16,
                             speech.sentence_batch_size);
        unsigned concurrency = 1;
        read_bounded_integer(section, "max_concurrency", "speech", 1, concurrency);
        if (speech.max_audio_bytes < 1024)
        {
            invalid("speech.max_audio_bytes");
        }

        std::string reference;
        read_field(section, "reference_audio", "speech", reference);
        std::string resources;
        read_field(document["resources"], "directory", "resources", resources);
        std::string receipts;
        read_field(document["viewers"], "receipt_directory", "viewers", receipts);
        const auto parent = config.config_path.parent_path();
        if (!reference.empty())
        {
            speech.reference_audio = parent / utf8_path(reference);
        }
        if (!resources.empty())
        {
            speech.resources_directory = parent / utf8_path(resources);
        }
        if (!receipts.empty())
        {
            speech.receipt_directory = parent / utf8_path(receipts);
        }
    }

    void read_viewers(toml::node_view<const toml::node> section, const std::filesystem::path& base,
                      meowlive::server::ViewerConfig& viewers)
    {
        validate_keys(section, "viewers",
                      {"enabled", "agent_memory", "scope_id", "database_url_env", "libpq_path",
                       "migrations_path", "calendar_offset_minutes", "receipt_directory"});
        read_field(section, "enabled", "viewers", viewers.enabled);
        read_field(section, "agent_memory", "viewers", viewers.agent_memory);
        read_field(section, "calendar_offset_minutes", "viewers", viewers.calendar_offset_minutes);
        if (viewers.calendar_offset_minutes < -840 || viewers.calendar_offset_minutes > 840)
        {
            invalid("viewers.calendar_offset_minutes");
        }
        read_field(section, "scope_id", "viewers", viewers.scope_id);
        read_field(section, "database_url_env", "viewers", viewers.database_url_env);
        if (viewers.scope_id.empty() || viewers.scope_id.size() > 128 ||
            !std::ranges::all_of(viewers.scope_id,
                                 [](unsigned char c)
                                 {
                                     return c >= 0x21 && c <= 0x7e;
                                 }))
        {
            invalid("viewers.scope_id");
        }
        if (viewers.database_url_env.empty() || viewers.database_url_env.size() > 128 ||
            !std::ranges::all_of(viewers.database_url_env,
                                 [](char c)
                                 {
                                     return c == '_' || (c >= 'a' && c <= 'z') ||
                                            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                                 }))
        {
            invalid("viewers.database_url_env");
        }
        std::string libpq;
        std::string migrations;
        read_field(section, "libpq_path", "viewers", libpq);
        read_field(section, "migrations_path", "viewers", migrations);
        if (!libpq.empty())
        {
            viewers.libpq = base / std::filesystem::path(std::u8string(libpq.begin(), libpq.end()));
        }
        if (!migrations.empty())
        {
            viewers.migrations =
                base / std::filesystem::path(std::u8string(migrations.begin(), migrations.end()));
        }
    }

    void read_memory(toml::node_view<const toml::node> section, bool viewers_enabled,
                     meowlive::server::MemoryConfig& memory)
    {
        validate_keys(section, "memory",
                      {"enabled", "endpoint", "model", "api_key_env", "embedding_endpoint",
                       "embedding_model", "embedding_api_key_env", "embedding_dimensions",
                       "timeout_ms"});
        read_field(section, "enabled", "memory", memory.enabled);
        read_field(section, "endpoint", "memory", memory.endpoint);
        read_field(section, "model", "memory", memory.model);
        read_field(section, "api_key_env", "memory", memory.api_key_env);
        read_field(section, "embedding_endpoint", "memory", memory.embedding_endpoint);
        read_field(section, "embedding_model", "memory", memory.embedding_model);
        read_field(section, "embedding_api_key_env", "memory", memory.embedding_api_key_env);
        read_bounded_integer(section, "embedding_dimensions", "memory", 4096,
                             memory.embedding_dimensions);
        read_bounded_integer(section, "timeout_ms", "memory", 60000, memory.timeout_ms);
        if (memory.enabled && (!viewers_enabled || memory.endpoint.empty() || memory.model.empty()))
        {
            invalid("memory.enabled / endpoint / model");
        }
        if (memory.embedding_endpoint.empty() != memory.embedding_model.empty() ||
            memory.embedding_model.size() > 256)
        {
            invalid("memory.embedding_endpoint / embedding_model");
        }
        if (memory.api_key_env.size() > 128 || memory.embedding_api_key_env.size() > 128 ||
            !valid_env_name(memory.api_key_env) || !valid_env_name(memory.embedding_api_key_env))
        {
            invalid("memory.api_key_env / embedding_api_key_env");
        }
    }

    void read_live(toml::node_view<const toml::node> section, meowlive::server::LiveConfig& live)
    {
        validate_keys(section, "live",
                      {"enabled", "app_id", "access_key_id_env", "access_key_secret_env",
                       "identity_code_env", "reconnect_initial_ms", "reconnect_max_ms"});
        read_field(section, "enabled", "live", live.enabled);
        std::int64_t app_id{};
        read_field(section, "app_id", "live", app_id);
        if (app_id < 0 || (live.enabled && app_id == 0))
        {
            invalid("live.app_id");
        }
        live.app_id = static_cast<std::uint64_t>(app_id);
        for (const auto [key, variable] :
             {std::pair{"access_key_id_env", &live.access_key_id_env},
              std::pair{"access_key_secret_env", &live.access_key_secret_env},
              std::pair{"identity_code_env", &live.identity_code_env}})
        {
            read_field(section, key, "live", *variable);
            if (variable->empty() || variable->size() > 128 ||
                ((*variable)[0] >= '0' && (*variable)[0] <= '9') ||
                !std::ranges::all_of(*variable,
                                     [](char c)
                                     {
                                         return c == '_' || (c >= 'a' && c <= 'z') ||
                                                (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                                     }))
            {
                invalid(std::string("live.") + key);
            }
        }
        auto initial = static_cast<unsigned>(live.retry.initial.count());
        auto maximum = static_cast<unsigned>(live.retry.max.count());
        read_bounded_integer(section, "reconnect_initial_ms", "live", 60'000, initial);
        read_bounded_integer(section, "reconnect_max_ms", "live", 300'000, maximum);
        if (initial < 10)
        {
            invalid("live.reconnect_initial_ms");
        }
        if (maximum < initial)
        {
            invalid("live.reconnect_max_ms");
        }
        live.retry = {std::chrono::milliseconds(initial), std::chrono::milliseconds(maximum)};
    }
}
