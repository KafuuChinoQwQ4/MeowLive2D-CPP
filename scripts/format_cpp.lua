import("core.project.project")
import("lib.detect.find_tool")

function main()
    local root = project.directory()
    local arguments = {"-i", "--style=file"}
    for _, directory in ipairs({"apps", "libs", "adapters", "tests"}) do
        for _, extension in ipairs({"cpp", "cc", "cxx", "cppm", "ixx", "h", "hh", "hpp", "hxx"}) do
            for _, file in ipairs(os.files(path.join(root, directory, "**." .. extension))) do
                if not os.islink(file) then
                    table.insert(arguments, path.relative(file, root))
                end
            end
        end
    end

    if #arguments == 2 then
        return
    end
    local formatter = assert(find_tool("clang-format"), "Install clang-format to format the C++ sources.")
    os.vrunv(formatter.program, arguments, {curdir = root})
    print("Formatted %d C++ files with clang-format.", #arguments - 2)
end
