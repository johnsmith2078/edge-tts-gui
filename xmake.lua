add_rules("mode.debug", "mode.release")

-- Force MinGW platform (Qt 6.8.1 is MinGW-compiled, not MSVC)
set_plat("mingw")
set_arch("x86_64")
set_config("mingw", "D:\\Qt\\Tools\\mingw1310_64")
set_config("qt", "D:\\Qt\\6.8.1\\mingw_64")

-- Qt 6.8.1 MinGW 64-bit SDK paths
local qt_bin_dir = "D:\\Qt\\6.8.1\\mingw_64\\bin"
local qt_inc_dir = "D:\\Qt\\6.8.1\\mingw_64\\include"
local qt_lib_dir = "D:\\Qt\\6.8.1\\mingw_64\\lib"

-- Packaging tools
local windeployqt = qt_bin_dir .. "\\windeployqt6.exe"
local enigmavb    = "D:\\Program Files (x86)\\Enigma Virtual Box\\enigmavbconsole.exe"

target("edge-tts-gui")
    set_kind("binary")

    -- Project source include path
    add_includedirs("src")

    -- Qt include paths
    add_includedirs(qt_inc_dir)
    add_includedirs(qt_inc_dir .. "\\QtCore")
    add_includedirs(qt_inc_dir .. "\\QtGui")
    add_includedirs(qt_inc_dir .. "\\QtWidgets")
    add_includedirs(qt_inc_dir .. "\\QtWebSockets")
    add_includedirs(qt_inc_dir .. "\\QtMultimedia")
    add_includedirs(qt_inc_dir .. "\\QtNetwork")

    -- Qt library paths
    add_linkdirs(qt_lib_dir)

    -- Qt modules
    add_links("Qt6Widgets")
    add_links("Qt6Core")
    add_links("Qt6Gui")
    add_links("Qt6WebSockets")
    add_links("Qt6Multimedia")
    add_links("Qt6Network")

    -- Source files
    add_files("src/main.cpp")
    add_files("src/dialog.cpp")
    add_files("src/communicate.cpp")
    add_files("src/streamingprogressbar.cpp")
    add_files("src/dialog.h")
    add_files("src/communicate.h")
    add_files("src/streamingprogressbar.h")
    add_files("src/dialog.ui")
    add_files("src/resources.qrc")

    -- Windows-specific (suppress console window, link user32)
    add_links("user32")
    add_cxflags("-mwindows")
    add_ldflags("-mwindows", "-Wl,--subsystem,windows")

    -- Qt build rules: MOC (meta-object), UIC (UI form), RCC (resource)
    add_rules("qt.moc", { moc = qt_bin_dir .. "\\moc.exe" })
    add_rules("qt.ui", { uic = qt_bin_dir .. "\\uic.exe" })
    add_rules("qt.qrc", { rcc = qt_bin_dir .. "\\rcc.exe" })

    set_languages("c++17")

    -- Pre-build: generate .rc and compile Windows resource (app icon)
    before_build(function (target)
        import("core.base.option")

        local rc_src = path.join(os.projectdir(), "src", "resource.rc")
        local rc_obj = path.join(target:targetdir(), "resource.o")

        -- Auto-generate resource.rc if it doesn't exist
        if not os.isfile(rc_src) then
            local ico_path = path.join(os.projectdir(), "src", "favicon.ico")
            if os.isfile(ico_path) then
                print("[res] generating resource.rc ...")
                local f = io.open(rc_src, "w")
                if f then
                    f:write('IDI_ICON1 ICON "' .. path.absolute(ico_path):gsub("\\", "\\\\") .. '"')
                    f:close()
                end
            end
        end

        -- Compile .rc → .o with windres (MinGW)
        if os.isfile(rc_src) then
            print("[res] windres resource.rc ...")
            os.mkdir(path.directory(rc_obj))
            os.runv("windres", { rc_src, rc_obj,
                "-I", path.join(os.projectdir(), "src") })
            target:add("ldflags", rc_obj)
        end
    end)

    -- ======================================================================
    -- Post-build: windeployqt6 → copy RapidOCR → Enigma Virtual Box → single .exe
    -- ======================================================================
    after_build(function (target)
        import("core.base.option")

        if is_mode("debug") then
            print("[pack] skipped (debug mode)")
            return
        end

        local bindir   = target:targetdir()
        local exe_name = target:name() .. ".exe"
        local src_exe  = path.join(bindir, exe_name)

        -- Step 1: staging folder for windeployqt6
        local deploy_dir = path.join(bindir, "_deploy")
        os.tryrm(deploy_dir)
        os.mkdir(deploy_dir)
        os.cp(src_exe, deploy_dir)

        local deploy_exe = path.join(deploy_dir, exe_name)

        -- Step 2: windeployqt6 — collect Qt DLLs and plugins
        print("[pack] windeployqt6 ...")
        os.runv(windeployqt, {
            deploy_exe,
            "--no-translations",
            "--no-compiler-runtime",
            "--no-opengl-sw"
        })

        -- Step 3: copy RapidOCR into deploy dir (so it's bundled in virtual fs)
        local rapidocr_src = path.join(os.projectdir(), "build", "dist", "RapidOCR")
        local rapidocr_dst = path.join(deploy_dir, "RapidOCR")
        if os.isdir(rapidocr_src) then
            print("[pack] copying RapidOCR ...")
            os.cp(rapidocr_src, rapidocr_dst)
            print("[pack] RapidOCR copied to " .. rapidocr_dst)
        else
            print("[pack] WARNING: RapidOCR source not found at " .. rapidocr_src)
        end

        -- ==================================================================
        -- Recursively scan deploy_dir → EVB XML nodes
        -- ==================================================================
        local function indent(n)
            return string.rep("\t", n)
        end

        -- Collect entries; each = {etype=2|3, name, [children]}
        local function scan(scan_dir)
            local entries = {}
            -- Dirs first
            for _, d in ipairs(os.dirs(path.join(scan_dir, "*"))) do
                local name = path.filename(d)
                if name ~= "." and name ~= ".." then
                    table.insert(entries, {
                        etype    = 3,        -- directory
                        name     = name,
                        children = scan(d)
                    })
                end
            end
            -- Then files
            for _, f in ipairs(os.files(path.join(scan_dir, "*"))) do
                local name = path.filename(f)
                if name ~= exe_name then    -- skip the main exe
                    table.insert(entries, {
                        etype = 2,           -- regular file
                        name  = name,
                    })
                end
            end
            return entries
        end

        -- Write <File> XML lines for a single entry (file or dir)
        local function write_file(e, depth, disk_dir)
            local s  = indent(depth) .. "<File>\n"
            depth = depth + 1
            s = s .. indent(depth) .. "<Type>" .. e.etype .. "</Type>\n"
            s = s .. indent(depth) .. "<Name>" .. e.name .. "</Name>\n"
            if e.etype == 2 then
                -- Absolute on-disk path
                local disk_path = path.join(disk_dir, e.name)
                disk_path = path.absolute(disk_path)
                s = s .. indent(depth) .. "<File>" .. disk_path .. "</File>\n"
                s = s .. indent(depth) .. "<ActiveX>False</ActiveX>\n"
                s = s .. indent(depth) .. "<ActiveXInstall>False</ActiveXInstall>\n"
                s = s .. indent(depth) .. "<Action>0</Action>\n"
                s = s .. indent(depth) .. "<OverwriteDateTime>False</OverwriteDateTime>\n"
                s = s .. indent(depth) .. "<OverwriteAttributes>False</OverwriteAttributes>\n"
                s = s .. indent(depth) .. "<PassCommandLine>False</PassCommandLine>\n"
                s = s .. indent(depth) .. "<HideFromDialogs>0</HideFromDialogs>\n"
            else
                -- Type 3 (directory)
                s = s .. indent(depth) .. "<Action>0</Action>\n"
                s = s .. indent(depth) .. "<OverwriteDateTime>False</OverwriteDateTime>\n"
                s = s .. indent(depth) .. "<OverwriteAttributes>False</OverwriteAttributes>\n"
                s = s .. indent(depth) .. "<HideFromDialogs>0</HideFromDialogs>\n"
                if #e.children > 0 then
                    s = s .. indent(depth) .. "<Files>\n"
                    local child_dir = path.join(disk_dir, e.name)
                    for _, child in ipairs(e.children) do
                        s = s .. write_file(child, depth + 1, child_dir)
                    end
                    s = s .. indent(depth) .. "</Files>\n"
                else
                    s = s .. indent(depth) .. "<Files></Files>\n"
                end
            end
            s = s .. indent(depth - 1) .. "</File>\n"
            return s
        end

        local root_entries = scan(deploy_dir)

        -- Build the %DEFAULT FOLDER% files XML
        local files_xml = ""
        for _, e in ipairs(root_entries) do
            files_xml = files_xml .. write_file(e, 4, deploy_dir)
        end

        local boxed_exe = path.join(bindir, target:name() .. "_boxed.exe")
        local evb_file  = path.join(bindir, target:name() .. ".evb")

        -- Step 4: assemble EVB XML (matching GUI output: windows-1252 encoding, True/False)
        local evb_text = table.concat({
            '<?xml version="1.0" encoding="windows-1252"?>',
            "<>",
            "\t<InputFile>" .. path.absolute(deploy_exe) .. "</InputFile>",
            "\t<OutputFile>" .. path.absolute(boxed_exe) .. "</OutputFile>",
            "\t<Files>",
            "\t\t<Enabled>True</Enabled>",
            "\t\t<DeleteExtractedOnExit>False</DeleteExtractedOnExit>",
            "\t\t<CompressFiles>True</CompressFiles>",
            "\t\t<Files>",
            "\t\t\t<File>",
            "\t\t\t\t<Type>3</Type>",
            "\t\t\t\t<Name>%DEFAULT FOLDER%</Name>",
            "\t\t\t\t<Action>0</Action>",
            "\t\t\t\t<OverwriteDateTime>False</OverwriteDateTime>",
            "\t\t\t\t<OverwriteAttributes>False</OverwriteAttributes>",
            "\t\t\t\t<HideFromDialogs>0</HideFromDialogs>",
            "\t\t\t\t<Files>",
            files_xml,
            "\t\t\t\t</Files>",
            "\t\t\t</File>",
            "\t\t</Files>",
            "\t</Files>",
            "\t<Registries>",
            "\t\t<Enabled>False</Enabled>",
            "\t\t<Registries>",
            "\t\t\t<Registry>",
            "\t\t\t\t<Type>1</Type>",
            "\t\t\t\t<Virtual>True</Virtual>",
            "\t\t\t\t<Name>Classes</Name>",
            "\t\t\t\t<ValueType>0</ValueType>",
            "\t\t\t\t<Value></Value>",
            "\t\t\t\t<Registries></Registries>",
            "\t\t\t</Registry>",
            "\t\t\t<Registry>",
            "\t\t\t\t<Type>1</Type>",
            "\t\t\t\t<Virtual>True</Virtual>",
            "\t\t\t\t<Name>User</Name>",
            "\t\t\t\t<ValueType>0</ValueType>",
            "\t\t\t\t<Value></Value>",
            "\t\t\t\t<Registries></Registries>",
            "\t\t\t</Registry>",
            "\t\t\t<Registry>",
            "\t\t\t\t<Type>1</Type>",
            "\t\t\t\t<Virtual>True</Virtual>",
            "\t\t\t\t<Name>Machine</Name>",
            "\t\t\t\t<ValueType>0</ValueType>",
            "\t\t\t\t<Value></Value>",
            "\t\t\t\t<Registries></Registries>",
            "\t\t\t</Registry>",
            "\t\t\t<Registry>",
            "\t\t\t\t<Type>1</Type>",
            "\t\t\t\t<Virtual>True</Virtual>",
            "\t\t\t\t<Name>Users</Name>",
            "\t\t\t\t<ValueType>0</ValueType>",
            "\t\t\t\t<Value></Value>",
            "\t\t\t\t<Registries></Registries>",
            "\t\t\t</Registry>",
            "\t\t\t<Registry>",
            "\t\t\t\t<Type>1</Type>",
            "\t\t\t\t<Virtual>True</Virtual>",
            "\t\t\t\t<Name>Config</Name>",
            "\t\t\t\t<ValueType>0</ValueType>",
            "\t\t\t\t<Value></Value>",
            "\t\t\t\t<Registries></Registries>",
            "\t\t\t</Registry>",
            "\t\t</Registries>",
            "\t</Registries>",
            "\t<Packaging>",
            "\t\t<Enabled>False</Enabled>",
            "\t</Packaging>",
            "\t<Options>",
            "\t\t<ShareVirtualSystem>True</ShareVirtualSystem>",
            "\t\t<MapExecutableWithTemporaryFile>True</MapExecutableWithTemporaryFile>",
            "\t\t<TemporaryFileMask></TemporaryFileMask>",
            "\t\t<AllowRunningOfVirtualExeFiles>True</AllowRunningOfVirtualExeFiles>",
            "\t\t<ProcessesOfAnyPlatforms>False</ProcessesOfAnyPlatforms>",
            "\t</Options>",
            "\t<Storage>",
            "\t\t<Files>",
            "\t\t\t<Enabled>False</Enabled>",
            "\t\t\t<Folder>%DEFAULT FOLDER%\\</Folder>",
            "\t\t\t<RandomFileNames>False</RandomFileNames>",
            "\t\t\t<EncryptContent>False</EncryptContent>",
            "\t\t</Files>",
            "\t</Storage>",
            "</>",
            ""
        }, "\n")

        -- Step 5: write .evb file as plain text (windows-1252/ASCII, matching GUI)
        local f = io.open(evb_file, "w")
        if not f then
            print("[pack] ERROR: cannot open " .. evb_file .. " for writing")
            return
        end
        f:write(evb_text)
        f:close()

        print("[pack] generated " .. evb_file)

        -- Step 6: Enigma Virtual Box
        if os.isfile(enigmavb) then
            print("[pack] enigmavbconsole ...")
            os.runv(enigmavb, { path.absolute(evb_file) })
            if os.isfile(boxed_exe) then
                print("[pack] → " .. boxed_exe)
            else
                print("[pack] ERROR: boxed exe not created")
            end
        else
            print("[pack] WARNING: Enigma Virtual Box not found at " .. enigmavb)
            print("[pack] dist ready at " .. deploy_dir)
        end
    end)
