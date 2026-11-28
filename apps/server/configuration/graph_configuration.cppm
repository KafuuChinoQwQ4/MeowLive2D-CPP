module;

#include <toml++/toml.h>

#include <meowlive/msvc_stl.hpp>

export module meowlive.server.graph_configuration;

import meowlive.standard;

export namespace meowlive::server
{
    struct GraphConfig
    {
        bool enabled{};
        std::string endpoint{"http://127.0.0.1:17474/db/neo4j/query/v2"};
        std::string user{"neo4j"};
        std::string password_env{"MEOWLIVE_NEO4J_PASSWORD"};
    };

    meowlive::server::GraphConfig read_graph(toml::node_view<const toml::node> section,
                                             bool viewers)
    {
        meowlive::server::GraphConfig config;
        const auto invalid = [](std::string_view field)
        {
            throw std::runtime_error("配置无效 [graph." + std::string(field) + "]");
        };
        if (!section)
        {
            return config;
        }
        if (!section.is_table())
        {
            invalid("config");
        }
        for (const auto& [key, value] : *section.as_table())
        {
            const auto name = key.str();
            if (name == "enabled")
            {
                if (const auto enabled = value.value_exact<bool>())
                {
                    config.enabled = *enabled;
                    continue;
                }
            }
            else
            {
                for (auto [field, destination] : std::array{
                         std::pair{"endpoint", &config.endpoint}, std::pair{"user", &config.user},
                         std::pair{"password_env", &config.password_env}})
                {
                    if (name == field)
                    {
                        if (const auto text = value.value_exact<std::string>();
                            text && !text->empty())
                        {
                            *destination = *text;
                            break;
                        }
                        invalid(field);
                    }
                }
                if (name == "endpoint" || name == "user" || name == "password_env")
                {
                    continue;
                }
            }
            invalid("config");
        }
        if (config.enabled && !viewers)
        {
            invalid("enabled");
        }
        return config;
    }
}
