package("toml++")
    set_homepage("https://github.com/marzer/tomlplusplus")
    set_description("Header-only TOML parser")
    set_license("MIT")
    set_kind("library", {headeronly = true})

    add_urls("https://github.com/marzer/tomlplusplus/archive/refs/tags/$(version).tar.gz")
    add_versions("v3.4.0", "8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155")

    on_install(function (package)
        os.cp("include/toml++", package:installdir("include"))
    end)
