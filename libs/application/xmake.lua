target("meowlive-application")
    set_kind("static")
    add_files("**.cppm", {public = true})
    add_deps("meowlive-domain", "meowlive-protocol")
