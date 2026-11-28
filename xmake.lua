set_xmakever("3.1.2")
set_project("MeowLive2D-cpp")
set_languages("cxx26")
add_includedirs("libs/platform/include")
set_policy("build.c++.modules", true)
if is_plat("windows") then
    set_policy("build.c++.modules.std", false)
    if is_host("linux") then
        -- Xmake 3.1.2's Clang BMI reuse check treats clang-cl's -external:I as target-specific.
        set_policy("package.include_external_headers", false)
    end
    -- Keep UTF-8 source handling identical for module producers and consumers.
    add_cxxflags("/utf-8", {tools = {"clang_cl", "cl"}})
end
add_rules("mode.debug", "mode.release")

task("format")
    set_menu({usage = "xmake format", description = "Format the project's C++ sources."})
    on_run(function ()
        import("core.project.project")
        import("format_cpp", {rootdir = path.join(os.projectdir(), "scripts")})
        project.lock()
        format_cpp()
    end)
task_end()

task("clangd")
    set_menu({usage = "xmake clangd", description = "Export the active build's compilation database for clangd."})
    on_run(function ()
        import("export_clangd", {rootdir = path.join(os.projectdir(), "scripts")})
        export_clangd()
    end)
task_end()

includes("libs/platform", "libs/protocol")

if is_plat("linux", "windows") then
    includes("packaging/drogon.lua")
    includes("packaging/tomlplusplus.lua")
    add_requires("drogon 1.9.14", {system = false})
    add_requires("jsoncpp 1.9.6", "toml++ v3.4.0", {system = false})
    add_requireconfs("drogon.jsoncpp", "drogon.zlib", {system = false})
    includes("libs/domain", "libs/application", "libs/agent", "adapters", "apps/server")
    includes("tests/contracts", "tests/integration", "tests/unit")
end

if is_plat("windows") then
    includes("libs/desktop-runtime", "apps/desktop", "tests/benchmarks")
end
