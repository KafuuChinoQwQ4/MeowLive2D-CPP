set_xmakever("3.1.2")
set_project("MeowLive2D-cpp")
set_languages("cxx26")
set_policy("build.c++.modules", true)
if is_plat("windows") then
    set_policy("build.c++.modules.std", false)
end
add_rules("mode.debug", "mode.release")

rule("meowlive.format")
    set_kind("project")
    after_build(function (opt)
        import("core.base.option")
        if (opt and opt.errors) or option.get("dry-run") then
            return
        end
        import("format_cpp", {rootdir = path.join(os.projectdir(), "scripts")})
        format_cpp()
    end)
rule_end()
add_rules("meowlive.format")

task("format")
    set_menu({usage = "xmake format", description = "Format the project's C++ sources."})
    on_run(function ()
        import("core.project.project")
        import("format_cpp", {rootdir = path.join(os.projectdir(), "scripts")})
        project.lock()
        format_cpp()
    end)
task_end()

includes("libs/protocol")

if is_plat("linux", "windows") then
    includes("packaging/drogon.lua")
    includes("packaging/tomlplusplus.lua")
    add_requires("drogon 1.9.14", {system = false})
    add_requires("jsoncpp 1.9.6", "toml++ v3.4.0", {system = false})
    add_requireconfs("drogon.jsoncpp", "drogon.zlib", {system = false})
    includes("libs/domain", "libs/application", "libs/agent", "adapters", "apps/server")
    includes("tests/contracts", "tests/integration")
end

if is_plat("windows") then
    includes("libs/desktop-runtime", "apps/desktop", "tests/benchmarks")
end
