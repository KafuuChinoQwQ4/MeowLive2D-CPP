#include <cstdio>
#include <meowlive/msvc_stl.hpp>

import meowlive.standard;

import meowlive.agent.interaction;
import meowlive.agent.session;
import meowlive.application.llm;
import meowlive.application.scheduler;
import meowlive.domain.live_event;
import meowlive.domain.speech;

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    meowlive::domain::LiveEvent event(std::string id, meowlive::domain::EventKind kind,
                                      std::uint64_t at, std::string viewer = {})
    {
        std::optional<meowlive::domain::ViewerIdentity> identity;
        if (!viewer.empty())
        {
            identity.emplace("room:fixture", meowlive::domain::ViewerIdentityKind::OpenId,
                             std::move(viewer));
        }
        return {std::move(id), "fixture", "同名观众", std::move(identity), at, {}, std::move(kind)};
    }

    meowlive::agent::AgentSession session(meowlive::agent::AgentLimits limits = {})
    {
        meowlive::agent::AgentSession agent(limits);
        meowlive::agent::AgentSettings settings;
        settings.cooldown_ms = 1000;
        settings.interaction.chat_mode = meowlive::agent::ChatMode::Selective;
        agent.configure(std::move(settings), 0);
        agent.set_paused(false, 0);
        return agent;
    }

    meowlive::agent::DecisionWork begin(meowlive::agent::AgentSession& agent, std::uint64_t now)
    {
        auto work = agent.begin(now);
        require(work.has_value() && !work->request.events.empty(), "Expected scheduled events");
        return std::move(*work);
    }

    void speak(meowlive::agent::AgentSession& agent, const meowlive::agent::DecisionWork& work,
               std::vector<std::string> ids, std::string text, std::uint64_t now,
               meowlive::domain::SpeechStatus terminal = meowlive::domain::SpeechStatus::Completed)
    {
        auto resolved = agent.resolve(work.id, {std::move(ids), std::move(text), {}}, now);
        require(resolved && resolved->has_value(), "Decision did not prepare speech");
        meowlive::application::SpeechTask task{
            .id = "speech", .generation = 1, .status = meowlive::domain::SpeechStatus::Queued};
        agent.enqueued(std::move(**resolved), task, now);
        task.status = terminal;
        agent.sync(task, now);
    }

    const meowlive::agent::EventRecord& record(const meowlive::agent::AgentSession& agent,
                                               std::string_view id)
    {
        const auto& records = agent.events();
        const auto row = std::ranges::find_if(records,
                                              [id](const auto& item)
                                              {
                                                  return item.event.id == id;
                                              });
        require(row != records.end(), "Expected event record");
        return *row;
    }

    void capacity()
    {
        auto agent = session({.pending_capacity = 2, .dedup_capacity = 2});
        agent.submit({event("chat", meowlive::domain::Chat{"弹幕"}, 100),
                      event("enter", meowlive::domain::RoomEnter{}, 100)},
                     100, 100);
        auto outcome =
            agent.submit({event("gift", meowlive::domain::Gift{"花", 1}, 200)}, 200, 200);
        require(outcome.accepted == 1 &&
                    record(agent, "enter").status == meowlive::agent::EventStatus::Skipped,
                "Gift should displace lowest-priority pending event");
        outcome =
            agent.submit({event("paid", meowlive::domain::SuperChat{"SC", 20, 200, 500}, 200),
                          event("same", meowlive::domain::Gift{"花", 1}, 200),
                          event("expired", meowlive::domain::SuperChat{"SC", 20, 50, 150}, 50)},
                         200, 200);
        require(outcome.accepted == 2 && outcome.unscheduled == 1 &&
                    record(agent, "chat").status == meowlive::agent::EventStatus::Skipped &&
                    record(agent, "expired").status == meowlive::agent::EventStatus::Expired,
                "Priority admission, expiry or full-queue accounting mismatch");
        const auto work = begin(agent, 200);
        require(work.request.events.front().id == "paid", "Paid message must run first");
        outcome =
            agent.submit({event("new-chat", meowlive::domain::Chat{"新弹幕"}, 200)}, 200, 200);
        require(outcome.accepted == 1 &&
                    record(agent, "paid").status == meowlive::agent::EventStatus::Deciding,
                "Decision in flight consumed a pending slot or was displaced");
        outcome = agent.submit({event("chat", meowlive::domain::Gift{"花", 1}, 200)}, 200, 200);
        require(outcome.accepted == 1 &&
                    record(agent, "chat").status == meowlive::agent::EventStatus::Pending &&
                    std::ranges::count_if(agent.events(),
                                          [](const auto& row)
                                          {
                                              return row.event.id == "chat";
                                          }) == 1,
                "Previously evicted dedup identifier did not replace the terminal row");
    }

    void ignored_target()
    {
        auto agent = session();
        for (std::uint64_t index = 0; index < 2; ++index)
        {
            const auto id = "gift-" + std::to_string(index);
            const auto now = index * 1000;
            agent.submit({event(id, meowlive::domain::Gift{"花", 1}, now, id)}, now, now);
            speak(agent, begin(agent, now), {id}, "谢谢！", now);
        }
        agent.submit({event("gift", meowlive::domain::Gift{"花", 1}, 2000, "donor"),
                      event("ordinary", meowlive::domain::Chat{"你好"}, 2000, "other")},
                     2000, 2000);
        const auto initial = begin(agent, 2000);
        require(initial.request.events.size() == 2 &&
                    initial.request.events.front().id == "ordinary",
                "Two completed gift turns should prioritize an ordinary chat");
        speak(agent, initial, {"gift"}, "谢谢！", 2000);
        agent.submit({event("gift-flood", meowlive::domain::Gift{"花", 1}, 3000)}, 3000, 3000);
        for (std::uint64_t now : {3000, 4000})
        {
            const auto work = begin(agent, now);
            require(work.request.events.size() == 1 && work.request.events.front().id == "ordinary",
                    "Ignored preferred chat should narrow the next attempt");
            const auto silent = agent.resolve(work.id, {}, now);
            require(silent && !silent->has_value(), "Selective mode may skip the proposed chat");
        }
        require(record(agent, "ordinary").status == meowlive::agent::EventStatus::Skipped,
                "Retry budget must be bounded");
        require(begin(agent, 5000).request.events.front().id == "gift-flood",
                "Retry budget should return to ordinary scheduling");
    }

    void gift_group()
    {
        auto agent = session();
        agent.submit({event("first", meowlive::domain::Gift{"花", 1}, 100, "donor"),
                      event("second", meowlive::domain::Gift{"花", 1}, 101, "donor")},
                     102, 102);
        const auto work = begin(agent, 102);
        require(work.request.events.size() == 2, "Expected both gifts to enter the decision");
        const auto partial = agent.resolve(work.id, {{"first"}, "谢谢！", {}}, 102);
        require(!partial && partial.error() == "incomplete_gift_group" &&
                    record(agent, "first").status == meowlive::agent::EventStatus::Deciding &&
                    record(agent, "second").status == meowlive::agent::EventStatus::Deciding,
                "A partial gift group must leave the current decision available for correction");
        speak(agent, work, {"first", "second"}, "谢谢！", 102);
        require(record(agent, "first").status == meowlive::agent::EventStatus::Completed &&
                    record(agent, "second").status == meowlive::agent::EventStatus::Completed,
                "Completed playback must update both gifts in the group");
    }

    void focus(std::string identity, std::string reply, std::uint64_t now, bool expected,
               meowlive::domain::SpeechStatus terminal)
    {
        auto agent = session();
        agent.submit({event("question", meowlive::domain::Chat{"你好"}, 0, "target")}, 0, 0);
        speak(agent, begin(agent, 0), {"question"}, "你喜欢橘、猫吗？", 0, terminal);
        agent.submit({event("thanks", meowlive::domain::Gift{"花", 1}, 1000, "other")}, 1000, 1000);
        speak(agent, begin(agent, 1000), {"thanks"}, "谢谢！", 1000);
        agent.submit(
            {event("reply", meowlive::domain::Chat{std::move(reply)}, now, std::move(identity)),
             event("gift", meowlive::domain::Gift{"花", 1}, now, "donor")},
            now, now);
        const auto work = begin(agent, now);
        require(work.request.events.front().id == (expected ? "reply" : "gift"),
                "Follow-up needs a completed recent question, stable identity and shared word");
        if (expected)
        {
            speak(agent, work, {"gift"}, "谢谢！", now);
            const auto retry = begin(agent, now + 1000);
            require(retry.request.events.size() == 1 && retry.request.events.front().id == "reply",
                    "Omitted focused reply should narrow the next attempt");
        }
    }
}

int main()
{
    try
    {
        using Status = meowlive::domain::SpeechStatus;
        capacity();
        ignored_target();
        gift_group();
        focus("target", "我喜欢橘猫", 2000, true, Status::Completed);
        focus("different", "我喜欢橘猫", 2000, false, Status::Completed);
        focus("", "我喜欢橘猫", 2000, false, Status::Completed);
        focus("target", "今天天气晴朗", 2000, false, Status::Completed);
        focus("target", "我喜欢橘猫", 120'000, false, Status::Completed);
        focus("target", "我喜欢橘猫", 2000, false, Status::Cancelled);
        std::println(
            "Agent priority, bounded retries, gift grouping, and conversation focus passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "Agent scheduling test failed: {}", error.what());
        return 1;
    }
}
