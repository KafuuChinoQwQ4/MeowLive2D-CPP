target("meowlive-unit-tests")
    set_kind("binary")
    set_default(false)
    add_files("work_queue.cpp")
    add_deps("meowlive-application")
    add_tests("work_queue")

target("meowlive-http-work-tests")
    set_kind("binary")
    set_default(false)
    add_files("http_work.cpp", "../../apps/server/transport/http/http_response.cppm",
              "../../apps/server/transport/http/http_work.cppm",
              "../../apps/server/transport/viewers/storage_http.cppm")
    add_deps("meowlive-adapters")
    add_packages("drogon")
    if is_plat("windows") then
        set_policy("build.c++.modules.two_phases", false)
    end
    add_tests("http_work", {run_timeout = 10000})

target("meowlive-speech-wav-tests")
    set_kind("binary")
    set_default(false)
    add_files("speech_wav.cpp")
    add_deps("meowlive-adapters")
    add_tests("speech_wav")

target("meowlive-agent-session-tests")
    set_kind("binary")
    set_default(false)
    add_files("agent_session.cpp")
    add_deps("meowlive-agent")
    add_tests("agent_session")

target("meowlive-live-session-tests")
    set_kind("binary")
    set_default(false)
    add_files("live_session.cpp")
    add_deps("meowlive-application")
    add_tests("live_session", {run_timeout = 30000})

target("meowlive-reply-completion-tests")
    set_kind("binary")
    set_default(false)
    add_files("reply_completion.cpp")
    add_deps("meowlive-application")
    add_tests("reply_completion", {run_timeout = 10000})

target("meowlive-memory-extraction-tests")
    set_kind("binary")
    set_default(false)
    add_files("memory_extraction.cpp")
    add_deps("meowlive-application")
    add_tests("memory_extraction")
