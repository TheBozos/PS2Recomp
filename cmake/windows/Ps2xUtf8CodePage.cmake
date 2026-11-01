# ps2x_utf8_code_page(<target>): the UTF-8 process code page for a Windows executable (cmake/windows/utf8.manifest).
# The tools and the runner open files through narrow APIs; with the system's ANSI code page a path with a character
# outside it (a TOML naming C:\Users\Jérôme\..., which is UTF-8) cannot be opened. MinGW: a resource script with the
# manifest (RT_MANIFEST 1, which takes the place of MSYS2's default manifest); MSVC embeds a .manifest source itself.
# The calling directory must have enabled the RC language for MinGW (enable_language(RC) at file scope).
option(PS2X_WINDOWS_UTF8 "Windows: give the tools and the runner the UTF-8 process code page" ON)

set(PS2X_UTF8_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/utf8.manifest")

function(ps2x_utf8_code_page target)
    if(NOT WIN32 OR NOT PS2X_WINDOWS_UTF8)
        return()
    endif()
    if(MSVC)
        target_sources(${target} PRIVATE "${PS2X_UTF8_MANIFEST}")
    elseif(MINGW)
        # The manifest is copied beside the script and named without a folder: windres opens the files a script names
        # through the ANSI code page, so an absolute path with a non-ASCII character (C:/Users/Jerome with an accent)
        # would not open; a bare name is found in the script's own folder.
        set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_utf8.rc")
        configure_file("${PS2X_UTF8_MANIFEST}" "${CMAKE_CURRENT_BINARY_DIR}/ps2x_utf8.manifest" COPYONLY)
        file(WRITE "${rc}.in" "// Made by Ps2xUtf8CodePage.cmake.\n1 24 \"ps2x_utf8.manifest\"\n")
        configure_file("${rc}.in" "${rc}" COPYONLY)
        target_sources(${target} PRIVATE "${rc}")
        set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/ps2x_utf8.manifest")
    endif()
endfunction()
