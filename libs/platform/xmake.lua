target("meowlive-platform")
    set_kind("static")
    add_files("standard.cppm", "exception_diagnostics.cppm", {public = true})
    if is_plat("windows") then
        add_files("windows/*.cppm", {public = true})
    end
