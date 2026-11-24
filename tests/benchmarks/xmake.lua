target("meowlive-playback-baseline")
    set_kind("binary")
    set_default(false)
    add_files("playback_baseline.cpp", "audio_loopback.cppm", "controlled_tts.cppm",
              "../integration/server_process.cppm", "../integration/transport_client.cppm")
    add_deps("meowlive-protocol")
    add_packages("drogon")
    add_syslinks("ole32", "uuid")
    add_cxxflags("/utf-8", {tools = {"clang_cl", "cl"}})
