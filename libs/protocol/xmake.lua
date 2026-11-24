target("meowlive-protocol")
    set_kind("static")
    add_files("*.cppm", {public = true})
    add_packages("jsoncpp", {public = true})
