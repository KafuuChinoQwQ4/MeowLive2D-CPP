module;

#include <meowlive/msvc_stl.hpp>

export module meowlive.domain.memory;

import meowlive.standard;

namespace
{
    constexpr std::int64_t day_ms = 86'400'000;

    enum class Kind
    {
        Preference,
        PreferredName,
        StableFact,
        Experience,
        TemporaryState,
        ThirdPartyClaim,
        SensitiveInference,
    };

    std::optional<Kind> kind_of(std::string_view name)
    {
        constexpr std::array kinds{std::pair{"preference", Kind::Preference},
                                   std::pair{"preferred_name", Kind::PreferredName},
                                   std::pair{"stable_fact", Kind::StableFact},
                                   std::pair{"experience", Kind::Experience},
                                   std::pair{"temporary_state", Kind::TemporaryState},
                                   std::pair{"third_party_claim", Kind::ThirdPartyClaim},
                                   std::pair{"sensitive_inference", Kind::SensitiveInference}};
        const auto found = std::ranges::find_if(kinds,
                                                [name](const auto& pair)
                                                {
                                                    return pair.first == name;
                                                });
        return found != kinds.end() ? std::optional(found->second) : std::nullopt;
    }

    bool exact_statement(std::string_view quote, std::string_view value, std::string_view prefix,
                         std::string_view suffix = {})
    {
        return quote.size() == prefix.size() + value.size() + suffix.size() &&
               quote.starts_with(prefix) && quote.ends_with(suffix) &&
               quote.substr(prefix.size(), value.size()) == value;
    }

    bool self_report(Kind kind, std::string_view quote, std::string_view value)
    {
        switch (kind)
        {
        case Kind::Preference:
            return exact_statement(quote, value, "我比较喜欢") ||
                   exact_statement(quote, value, "我觉得", "不错") ||
                   exact_statement(quote, value, "我喜欢");
        case Kind::PreferredName:
            return exact_statement(quote, value, "我一般被叫作") ||
                   exact_statement(quote, value, "我的昵称是");
        case Kind::StableFact:
            return exact_statement(quote, value, "我养") ||
                   exact_statement(quote, value, "我的职业是");
        case Kind::Experience:
            return exact_statement(quote, value, "我昨天去了") ||
                   exact_statement(quote, value, "我今天去了") ||
                   exact_statement(quote, value, "我参加了");
        case Kind::TemporaryState:
            return exact_statement(quote, value, "我今天很") ||
                   exact_statement(quote, value, "我现在很");
        default:
            return false;
        }
    }

    bool clear_statement(Kind kind, std::string_view quote, std::string_view value)
    {
        if (kind == Kind::Preference)
        {
            return exact_statement(quote, value, "我喜欢") ||
                   exact_statement(quote, value, "我最喜欢");
        }
        return kind == Kind::PreferredName &&
               (exact_statement(quote, value, "请叫我") || exact_statement(quote, value, "叫我"));
    }

    bool contains_any(std::string_view text, std::initializer_list<std::string_view> words)
    {
        return std::ranges::any_of(words,
                                   [text](std::string_view word)
                                   {
                                       return text.contains(word);
                                   });
    }
}

export namespace meowlive::domain
{
    struct MemoryStatement
    {
        std::string_view quote;
        std::int64_t occurred_at_ms{};
        std::int64_t day{};
        bool complete{};
    };

    struct MemoryAssessment
    {
        std::string status;
        std::optional<std::int64_t> expires_at_ms;
    };

    // The application has already checked these statements against their original sources.
    std::optional<meowlive::domain::MemoryAssessment>
    classify_memory(std::string_view name, std::string_view value,
                    std::span<const meowlive::domain::MemoryStatement> statements,
                    std::optional<std::int64_t> valid_until_ms, std::int64_t now)
    {
        const auto kind = kind_of(name);
        if (!kind || statements.empty())
        {
            return std::nullopt;
        }
        std::set<std::int64_t> days;
        auto oldest = now;
        bool clear = true;
        bool reported = true;
        for (const auto& statement : statements)
        {
            days.insert(statement.day);
            oldest = std::min(oldest, statement.occurred_at_ms);
            reported &=
                statement.complete && self_report(*kind, statement.quote, value) &&
                !contains_any(statement.quote, {"如果", "假如", "可能", "也许", "听说", "他说",
                                                "她说", "不是", "不喜欢", "讨厌", "开玩笑", "骗",
                                                "抑郁", "疾病", "政治", "宗教", "性取向", "诊断"});
            clear &= statement.complete && clear_statement(*kind, statement.quote, value) &&
                     !contains_any(value, {"如果", "假如", "可能", "也许", "听说",   "开玩笑", "骗",
                                           "抑郁", "疾病", "政治", "宗教", "性取向", "诊断",   "但",
                                           "不",   "，",   ",",    "。",   ";",      "；"});
        }

        if (valid_until_ms && *valid_until_ms < oldest)
        {
            return std::nullopt;
        }
        std::string status = "candidate";
        std::int64_t ttl = 7 * day_ms;
        if (clear || (reported && days.size() >= 2 &&
                      (*kind == Kind::Preference || *kind == Kind::PreferredName ||
                       *kind == Kind::StableFact)))
        {
            status = "long_term";
            ttl = 0;
        }
        else if (reported && (*kind == Kind::Experience || *kind == Kind::TemporaryState))
        {
            status = "short_term";
            ttl = *kind == Kind::TemporaryState ? day_ms : 30 * day_ms;
        }

        auto expiry = valid_until_ms;
        if (ttl != 0)
        {
            const auto latest = std::numeric_limits<std::int64_t>::max();
            const auto deadline = oldest > latest - ttl ? latest : oldest + ttl;
            expiry = std::min(deadline, expiry.value_or(deadline));
        }
        return meowlive::domain::MemoryAssessment{
            expiry && *expiry <= now ? "expired" : std::move(status), expiry};
    }
}
