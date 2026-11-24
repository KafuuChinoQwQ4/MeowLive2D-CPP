target("meowlive-agent")
    set_kind("static")
    add_files("*.cppm", {public = true})
    add_deps("meowlive-application")
