add_requires("libsodium 1.0.22", {system = false})
local update_public_key = os.getenv("MEOWLIVE_UPDATE_PUBLIC_KEY") or ""
local update_version = os.getenv("MEOWLIVE_RELEASE_TAG") or "0.1.4"
if not update_public_key:match("^[A-Za-z0-9+/=]*$") then
    update_public_key = ""
end
if not update_version:match("^[A-Za-z0-9.-]+$") then
    update_version = "0.1.4"
end

target("meowlive-desktop")
    add_rules("qt.widgetapp")
    -- Clang's MSVC ABI crashes on Qt member pointers when emitting objects from a full BMI.
    set_policy("build.c++.modules.two_phases", false)
    add_frameworks("QtNetwork", "QtWebSockets", "QtEntryPoint")
    add_syslinks("winmm")
    add_files("main.cpp", "ui/**.cppm", "ui/**.cpp", "services/*.cppm")
    add_deps("meowlive-desktop-runtime")

target("meowlive-update")
    set_policy("build.c++.modules.two_phases", false)
    add_rules("qt.console")
    add_frameworks("QtNetwork")
    add_files("update/*.cppm", "update/main.cpp")
    add_deps("meowlive-platform")
    add_packages("libsodium")
    add_defines('MEOWLIVE_UPDATE_PUBLIC_KEY="' .. update_public_key .. '"')
    add_defines('MEOWLIVE_VERSION="' .. update_version .. '"')
