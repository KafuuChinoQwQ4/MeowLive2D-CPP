target("meowlive-server")
    set_kind("binary")
    add_files("**.cpp", "**.cppm")
    add_deps("meowlive-application", "meowlive-agent", "meowlive-adapters", "meowlive-protocol")
    add_packages("drogon", "toml++")
    if is_plat("windows") then
        set_policy("build.c++.modules.two_phases", false)
    end
