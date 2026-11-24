target("meowlive-server")
    set_kind("binary")
    add_files("main.cpp", "*.cppm", "transport/*.cppm")
    add_deps("meowlive-agent", "meowlive-adapters", "meowlive-protocol")
    add_packages("drogon", "toml++")
    if is_plat("windows") then
        add_cxxflags("/utf-8", {tools = {"clang_cl", "cl"}})
    end
