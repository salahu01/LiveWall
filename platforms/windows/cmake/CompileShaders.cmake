# Compiles HLSL entry points to byte-code headers at build time.
#
# The runtime alternative — shipping the .hlsl and calling D3DCompile on
# startup — pulls the shader compiler DLL into the process. On macOS the
# equivalent (compiling a .metal at runtime instead of loading a .metallib)
# measured at ~97 MB of resident graphics memory that is never released, and it
# is the single largest cost the app avoids. Same trade here, so the same
# answer: compile ahead of time, embed the byte code, link nothing extra.
#
# fxc.exe ships with the Windows SDK, so this adds no dependency.

find_program(LIVEWALL_FXC
    NAMES fxc fxc.exe
    HINTS
        "$ENV{WindowsSdkVerBinPath}x64"
        "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    DOC "HLSL compiler from the Windows SDK")

# ENTRIES are "EntryPoint:profile" pairs. Each becomes
# <output>/<EntryPoint>.h defining a g_<EntryPoint> byte array.
function(livewall_add_shaders target)
    cmake_parse_arguments(SH "" "SOURCE;OUTPUT" "ENTRIES" ${ARGN})

    if(NOT LIVEWALL_FXC)
        message(FATAL_ERROR
            "fxc.exe was not found. It ships with the Windows SDK; open a "
            "Developer Command Prompt, or set LIVEWALL_FXC to its full path.")
    endif()

    file(MAKE_DIRECTORY ${SH_OUTPUT})
    set(outputs "")

    foreach(entry ${SH_ENTRIES})
        string(REPLACE ":" ";" parts ${entry})
        list(GET parts 0 name)
        list(GET parts 1 profile)

        set(header "${SH_OUTPUT}/${name}.h")

        # /Ges (strict) and /WX turn a shader that only happens to compile into
        # a build failure, which is the right time to find out.
        add_custom_command(
            OUTPUT ${header}
            COMMAND ${LIVEWALL_FXC}
                    /nologo /T ${profile} /E ${name}
                    /O3 /Ges /WX
                    /Vn g_${name}
                    /Fh ${header}
                    ${CMAKE_CURRENT_SOURCE_DIR}/${SH_SOURCE}
            MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/${SH_SOURCE}
            COMMENT "fxc ${name} (${profile})"
            VERBATIM)

        list(APPEND outputs ${header})
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
