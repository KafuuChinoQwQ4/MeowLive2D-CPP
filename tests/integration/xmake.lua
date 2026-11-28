target("meowlive-test-transport")
    set_kind("static")
    set_default(false)
    add_files("support/*.cppm", {public = true})
    add_deps("meowlive-protocol")
    add_packages("drogon", {public = true})

target("meowlive-transport-integration")
    set_kind("binary")
    set_default(false)
    add_files("transport/stage_2_transport.cpp", "transport/stage_2_startup.cppm")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    end
    set_rundir("$(projectdir)")
    add_tests("stage_2_transport", {run_timeout = 60000})

target("meowlive-playback-integration")
    set_kind("binary")
    set_default(false)
    add_files("playback/stage_3_playback.cpp", "playback/fixture.cppm",
              "../benchmarks/controlled_tts.cppm")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    else
        -- Windows runs the test and prebuilt server together on the native host.
        add_files("../benchmarks/audio_loopback.cppm")
        add_syslinks("ole32", "uuid")
    end
    set_rundir("$(projectdir)")
    add_tests("stage_3_playback", {run_timeout = 60000})

if is_plat("windows") then
    target("meowlive-desktop-playback-integration")
        set_default(false)
        add_rules("qt.console")
        add_frameworks("QtNetwork", "QtWebSockets")
        add_files("playback/stage_3_desktop.cpp")
        add_deps("meowlive-desktop-runtime")
        add_tests("stage_3_desktop", {run_timeout = 40000})

    target("meowlive-desktop-update-integration")
        set_default(false)
        set_policy("build.c++.modules.two_phases", false)
        add_rules("qt.console")
        add_frameworks("QtNetwork")
        add_files("windows/update_readiness.cpp", "../../apps/desktop/services/managed_server.cppm",
                  "../../apps/desktop/services/server_client.cppm",
                  "../../apps/desktop/services/update_readiness.cppm")
        add_deps("meowlive-protocol")
        add_tests("desktop_update_readiness", {run_timeout = 40000})
end

target("meowlive-profile-integration")
    set_kind("binary")
    set_default(false)
    add_files("agent/stage_4_profiles.cpp")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    end
    add_tests("stage_4_profiles", {run_timeout = 60000})

target("meowlive-agent-integration")
    set_kind("binary")
    set_default(false)
    add_files("agent/stage_4_agent.cpp")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    end
    add_tests("stage_4_agent", {run_timeout = 60000})

target("meowlive-live-integration")
    set_kind("binary")
    set_default(false)
    add_files("live/stage_5_live.cpp")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    end
    add_tests("stage_5_live", {run_timeout = 60000})

target("meowlive-voice-integration")
    set_kind("binary")
    set_default(false)
    add_files("speech/stage_6_voices.cpp")
    add_deps("meowlive-test-transport")
    add_packages("drogon")
    if is_plat("linux") then
        add_deps("meowlive-server", {inherit = false})
    end
    add_tests("stage_6_voices", {run_timeout = 60000})

target("meowlive-postgres-integration")
    set_kind("binary")
    set_default(false)
    add_files("storage/postgres_migrations.cpp")
    add_deps("meowlive-adapters")

target("meowlive-relationship-projection-integration")
    set_kind("binary")
    set_default(false)
    add_files("storage/relationship_projection.cpp", "../../apps/server/viewers/graph_worker.cppm")
    add_deps("meowlive-adapters")

if is_plat("windows") then
    target("meowlive-vtube-studio-integration")
        set_default(false)
        add_rules("qt.console")
        add_frameworks("QtNetwork", "QtWebSockets")
        add_files("avatar/stage_6_vtube_studio.cpp")
        add_deps("meowlive-desktop-runtime")
        add_tests("stage_6_vtube_studio", {run_timeout = 30000})
end

if is_plat("windows") then
    target("meowlive-native-whisper-integration")
        set_kind("binary")
        set_default(false)
        add_files("speech/stage_8_native_whisper.cpp")
        add_deps("meowlive-adapters")
        add_tests("stage_8_native_whisper", {run_timeout = 30000})
end
