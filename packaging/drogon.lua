package("drogon")
    set_homepage("https://github.com/drogonframework/drogon")
    set_description("Drogon HTTP framework")
    set_license("MIT")

    add_urls("https://github.com/drogonframework/drogon/archive/refs/tags/v$(version).tar.gz")
    add_versions("1.9.14", "3831bfae74eb6b47965a424e5c9d593d5efa31e9f7e6a5e02d4754039facff5e")
    add_resources("1.9.14", "trantor",
                  "https://github.com/an-tao/trantor/archive/78e480f5e138acdc05f76c55a2716718cd846364.tar.gz",
                  "60487a296530a330d753fe3c5e492a885d043a5b8d7ffa4d9bdc96b2813a394b")

    add_deps("cmake", "jsoncpp 1.9.6", "zlib 1.3.1")
    add_links("drogon", "trantor")
    add_defines("DROGON_STATIC_DEFINE", "TRANTOR_STATIC_DEFINE")
    if is_plat("windows") then
        add_syslinks("ws2_32", "rpcrt4", "crypt32", "advapi32", "iphlpapi", "shlwapi")
    else
        add_syslinks("uuid", "pthread", "dl")
    end

    on_install(function (package)
        local trantor = os.dirs(path.join(package:resourcedir("trantor"), "trantor-*"))[1]
        os.cp(path.join(trantor, "*"), "trantor")
        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_CTL=OFF",
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_TESTING=OFF",
            "-DBUILD_ORM=OFF",
            "-DBUILD_BROTLI=OFF",
            "-DBUILD_YAML_CONFIG=OFF",
            "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON",
            "-DCMAKE_DISABLE_FIND_PACKAGE_c-ares=ON"
        }
        if package:is_plat("windows") then
            table.insert(configs, "-DUSE_STATIC_LIBS_ONLY=ON")
        else
            table.insert(configs, "-DCMAKE_DISABLE_FIND_PACKAGE_jsoncpp=ON")
        end
        import("package.tools.cmake").install(package, configs)
    end)
