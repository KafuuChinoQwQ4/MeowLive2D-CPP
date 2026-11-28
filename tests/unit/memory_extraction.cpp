#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.application.llm;
import meowlive.application.memory_extraction;

namespace
{
    namespace app = meowlive::application;
    constexpr std::int64_t day = 86'400'000;
    constexpr std::int64_t now = 10 * day;
    constexpr std::string_view preference =
        R"({"candidates":[{"key":"preference","value":"跑步","kind":"preference","evidence":[{"source":"bilibili","event_id":"ev-1","quote":"我最喜欢跑步"}],"explicit":true,"confidence":0.8,"valid_until_ms":null}]})";

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class FixtureModel final : public app::LanguageModel
    {
    public:
        std::expected<app::ModelTurn, app::ModelError>
        turn(const app::DecisionRequest&, const app::ModelOptions&, std::stop_token) override
        {
            throw std::logic_error("decision model called for memory extraction");
        }

        std::expected<std::string, app::ModelError>
        complete_json(std::string_view system, std::string_view input, std::stop_token) override
        {
            ++calls;
            require(system.contains("untrusted data"), "source text instruction absent");
            require(input.contains("ev-1") && input.contains("我最喜欢跑步") &&
                        !input.contains("private-viewer"),
                    "memory request used incorrect source identifiers");
            if (cancel)
            {
                cancel->request_stop();
            }
            return reply;
        }

        int calls{};
        std::string reply{preference};
        std::stop_source* cancel{};
    };

    void extraction()
    {
        FixtureModel model;
        const app::MemorySource source{"bilibili",     "private-viewer", "ev-1",
                                       "我最喜欢跑步", now - 1,          9};
        const auto empty = app::extract_memories(model, {}, now, {});
        require(empty && empty->empty() && model.calls == 0, "empty source used the model");

        const std::array sources{source};
        const auto result = app::extract_memories(model, sources, now, {});
        require(result && result->size() == 1 && result->front().status == "long_term" &&
                    result->front().value == "跑步" && result->front().explicit_claim &&
                    result->front().evidence.front().day == source.day &&
                    !result->front().expires_at_ms,
                "trusted explicit preference was not promoted");

        auto previous = model.calls;
        require(!app::extract_memories(model, std::array{source, source}, now, {}) &&
                    model.calls == previous,
                "duplicate source reached the model");

        auto another_viewer = source;
        another_viewer.viewer_id = "other-viewer";
        another_viewer.event_id = "ev-2";
        require(!app::extract_memories(model, std::array{source, another_viewer}, now, {}) &&
                    model.calls == previous,
                "mixed viewers reached the model");

        model.reply.replace(model.reply.find("ev-1"), 4, "ev-2");
        require(!app::extract_memories(model, sources, now, {}), "invented event was accepted");
        model.reply = preference;
        model.reply.replace(model.reply.find("我最喜欢跑步"),
                            std::string_view("我最喜欢跑步").size(), "我喜欢跑步");
        require(!app::extract_memories(model, sources, now, {}), "invented quote was accepted");
        model.reply = preference;
        model.reply.replace(model.reply.find("null"), 4, "900000000");
        require(!app::extract_memories(model, sources, now, {}), "invented deadline was accepted");

        std::stop_source stop_source;
        previous = model.calls;
        stop_source.request_stop();
        require(!app::extract_memories(model, sources, now, stop_source.get_token()) &&
                    model.calls == previous,
                "stopped extraction reached the model");
        std::stop_source cancel_during_request;
        model.reply = preference;
        model.cancel = &cancel_during_request;
        require(!app::extract_memories(model, sources, now, cancel_during_request.get_token()) &&
                    model.calls == previous + 1,
                "cancelled extraction returned data");
    }

    void lifecycle()
    {
        const std::array facts{
            app::MemorySource{"bilibili", "viewer", "a", "我养猫", now - 2 * day, 8},
            app::MemorySource{"bilibili", "viewer", "b", "我养猫", now - day, 9}};
        app::MemoryCandidate fact{.key = "stable_fact",
                                  .value = "猫",
                                  .kind = "stable_fact",
                                  .evidence = {{"bilibili", "a", "我养猫", now - 2 * day, 8}},
                                  .confidence = 0.9};
        require(app::assess_memory(fact, facts, now) && fact.status == "candidate" &&
                    fact.expires_at_ms == now + 5 * day,
                "single stable fact retained the wrong status or expiry");
        fact.evidence.push_back({"bilibili", "b", "我养猫", now - day, 9});
        require(app::assess_memory(fact, facts, now) && fact.status == "long_term" &&
                    !fact.expires_at_ms,
                "cross-day stable fact was not promoted");
        fact.valid_until_ms = now;
        require(app::assess_memory(fact, facts, now) && fact.status == "expired" &&
                    fact.expires_at_ms == now,
                "trusted deadline did not expire promoted memory");

        const std::array temporary{
            app::MemorySource{"bilibili", "viewer", "c", "我现在很累", now - day, 9}};
        app::MemoryCandidate tired{.key = "temporary_state",
                                   .value = "累",
                                   .kind = "temporary_state",
                                   .evidence = {{"bilibili", "c", "我现在很累", now - day, 9}},
                                   .confidence = 0.7};
        require(app::assess_memory(tired, temporary, now - 1) && tired.status == "short_term" &&
                    tired.expires_at_ms == now && app::assess_memory(tired, temporary, now) &&
                    tired.status == "expired",
                "temporary state did not expire exactly after one day");
    }
}

int main()
{
    extraction();
    lifecycle();
}
