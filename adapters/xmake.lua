if is_plat("windows") then
    add_requires("libcurl 8.21.0", {system = false})
    add_requires("zlib 1.3.1", "brotli v1.2.0", {system = false})
else
    add_requires("libcurl >=8.16.0", "brotli", "openssl", {system = true})
    add_requires("zlib 1.3.1", {system = false})
end

target("meowlive-adapters")
    set_kind("static")
    add_files("**.cppm", {public = true})
    add_files("**.cpp")
    add_deps("meowlive-application", "meowlive-protocol")
    add_packages("libcurl", "zlib", "brotli", {public = true})
    if is_plat("windows") then
        add_syslinks("bcrypt", "ws2_32")
    else
        add_packages("openssl", {public = true})
    end
