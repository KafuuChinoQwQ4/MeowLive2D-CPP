import("core.base.json")
import("core.base.task")

function main()
    local root = os.projectdir()
    local staging = path.join(root, ".xmake", "clangd", path.filename(os.tmpfile()))
    os.mkdir(staging)
    try
    {
        function ()
            task.run("project", {kind = "compile_commands", lsp = "clangd", outputdir = staging})
            local filename = path.join(staging, "compile_commands.json")
            local commands = json.loadfile(filename)
            for _, command in ipairs(commands) do
                command.file = path.absolute(command.file, command.directory)
                local arguments = {}
                local index = 1
                while index <= #command.arguments do
                    local argument = command.arguments[index]
                    if argument == "-imsvc" then
                        -- Xmake 3.1.2 can export the Windows INCLUDE list as a single path on WSL.
                        index = index + 1
                        for _, directory in ipairs(command.arguments[index]:split(";", {plain = true})) do
                            table.insert(arguments, "-imsvc")
                            table.insert(arguments, directory)
                        end
                    else
                        table.insert(arguments, argument)
                    end
                    index = index + 1
                end
                command.arguments = arguments
            end
            json.savefile(filename, commands)
            os.mv(filename, path.join(root, "compile_commands.json"))
        end,
        finally
        {
            function (ok, errors)
                os.tryrm(staging)
                if not ok then
                    raise(errors)
                end
            end
        }
    }
end
