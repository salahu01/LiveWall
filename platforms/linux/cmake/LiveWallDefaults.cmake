# Compiler and preprocessor settings shared by every target.

function(livewall_set_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    target_compile_definitions(${target} PUBLIC
        _FILE_OFFSET_BITS=64
        # Asked for explicitly rather than left to the toolchain default:
        # getrandom(2), pipe2, memfd_create and the XDG-adjacent bits of
        # <stdlib.h> are all behind _GNU_SOURCE on glibc, and musl only
        # exposes some of them unconditionally.
        _GNU_SOURCE
    )

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wno-unused-parameter   # callback signatures are fixed; see below
        -fvisibility=hidden
    )

    # C++ only. wayland-scanner's generated marshalling is C, and gcc warns
    # about both of these on every one of those files.
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )

    target_compile_options(${target} PRIVATE
        $<$<CONFIG:Release>:-O2;-fno-plt;-ffunction-sections;-fdata-sections>
        $<$<CONFIG:Debug>:-O0;-g3>
    )

    # --gc-sections with the two -f flags above is what keeps this in the same
    # order of magnitude as the 504 KB macOS binary. --as-needed matters more
    # than usual here: nearly every library the app talks to is opened with
    # dlopen, and a stray -l on the link line would turn an optional runtime
    # dependency into a hard one that stops the app from starting.
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:-Wl,--gc-sections;-Wl,-O1>
        -Wl,--as-needed
        -Wl,-z,relro
        -Wl,-z,now
        -Wl,--no-undefined

        # The C++ runtime goes in the binary rather than being asked of the
        # system. Without this the floor is not the distro the binary was built
        # on but the compiler it was built with: a GCC 11 toolchain emits
        # references to GLIBCXX_3.4.29, so a binary built on Ubuntu 20.04 with a
        # newer GCC still refuses to start on a stock Ubuntu 20.04, with
        #
        #     version `GLIBCXX_3.4.29' not found
        #
        # and nothing about that is visible on the machine that built it.
        #
        # It costs roughly a megabyte after --gc-sections, which is the same
        # trade the rest of this file already makes: everything optional is
        # dlopen'd, everything required is carried. glibc itself stays dynamic —
        # static glibc breaks dlopen and NSS, which this app depends on.
        -static-libstdc++
        -static-libgcc
    )
endfunction()

# Turns a text file into a C++ header holding it as a string literal.
#
# Shaders are compiled by the GL driver at runtime whatever we do — there is no
# offline GLSL binary that is portable across Mesa, NVIDIA and the rest — so the
# thing worth avoiding is a *file* read at startup and an install layout that
# can go missing. Embedding costs a few kilobytes and removes both.
function(livewall_embed_text target)
    cmake_parse_arguments(ARG "" "OUTPUT" "SOURCES" ${ARGN})

    set(generated "")
    foreach(source ${ARG_SOURCES})
        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(extension "${source}" EXT)
        string(REPLACE "." "_" suffix "${extension}")
        set(symbol "k${name}${suffix}")
        set(header "${ARG_OUTPUT}/${name}${suffix}.h")

        add_custom_command(
            OUTPUT "${header}"
            COMMAND ${CMAKE_COMMAND}
                    -DINPUT=${source}
                    -DOUTPUT=${header}
                    -DSYMBOL=${symbol}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake
            DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake"
            COMMENT "Embedding ${name}${extension}"
            VERBATIM)

        list(APPEND generated "${header}")
    endforeach()

    add_custom_target(${target}_embedded_text DEPENDS ${generated})
    add_dependencies(${target} ${target}_embedded_text)
    target_include_directories(${target} PUBLIC "${ARG_OUTPUT}/..")
endfunction()
