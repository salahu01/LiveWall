# LiveWall for Linux.
#
# Linux has no equivalent of AVFoundation or Media Foundation — no media
# framework the OS guarantees is there — so the dependency question is answered
# differently here than on the other two platforms. It is answered by *when*
# rather than *whether*:
#
#   Linked   libX11, libXrandr, libEGL, libGLESv2, libwayland-client,
#            libwayland-egl. A desktop Linux machine has these or it has no
#            desktop. Together they are the display server and the GPU.
#
#   dlopen   libavformat, libavcodec, libavutil, libswscale, libva, libdbus-1,
#            libXss. Every one of these is optional at runtime: no ffmpeg means
#            no video (the procedural mode still runs), no libva means software
#            decode, no libdbus means no tray and no logind signals, no libXss
#            means no idle detection. The app starts and does something useful
#            in all of those cases, and says which ones it is missing.
#
# The -dev packages below are needed to *build* — for headers and for
# wayland-scanner — not to run. Nothing in the dlopen list appears on the link
# line, which is checked in CI with `ldd`.
cmake_minimum_required(VERSION 3.21)

project(LiveWall
    VERSION 1.0.0
    DESCRIPTION "Low-power live wallpaper for Linux"
    # C as well as C++: wayland-scanner emits C, and the generated protocol
    # marshalling is compiled straight into the core library.
    LANGUAGES CXX C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "platforms/linux targets Linux. For macOS use platforms/macos, for Windows platforms/windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(cmake/LiveWallDefaults.cmake)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Backends
#
# X11 is required and Wayland is opportunistic, which is the opposite of where
# the ecosystem is heading and is deliberate. The single feature this project
# exists for — tear the decoder down when nothing can see the wallpaper — needs
# to know what else is on screen, and X11 is the only one of the two where a
# client can find out. See README's "What Wayland costs".
# ---------------------------------------------------------------------------
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xrandr xext xfixes)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl glesv2)

option(LIVEWALL_WAYLAND "Build the wlr-layer-shell Wayland backend" ON)

set(LIVEWALL_WAYLAND_SOURCES "")
if(LIVEWALL_WAYLAND)
    pkg_check_modules(WAYLAND IMPORTED_TARGET wayland-client wayland-egl)
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(NOT WAYLAND_FOUND OR NOT WAYLAND_SCANNER)
        message(STATUS "Wayland backend off: need libwayland-dev and wayland-scanner")
        set(LIVEWALL_WAYLAND OFF)
    endif()
endif()

# ---------------------------------------------------------------------------
# Wayland protocol bindings
#
# The two XML files are vendored under protocol/ rather than read out of
# wayland-protocols' pkgdatadir. wlr-layer-shell is not in wayland-protocols at
# all (it ships with wlroots, which we do not otherwise need), and pinning
# xdg-output beside it means the build does not change shape depending on which
# version of wayland-protocols a distribution happens to carry.
# ---------------------------------------------------------------------------
if(LIVEWALL_WAYLAND)
    set(protocol_out "${CMAKE_BINARY_DIR}/generated/protocol")
    file(MAKE_DIRECTORY "${protocol_out}")

    # wlr-layer-shell names one of its arguments `namespace`, which
    # wayland-scanner reproduces verbatim in the generated C header. That is
    # legal C and will not compile as C++. Renaming the identifier after
    # generation is the fix; the alternative in circulation is
    # `#define namespace something` around the include, which is a macro over a
    # keyword and undefined behaviour in the standard it is trying to satisfy.
    function(livewall_wayland_protocol name)
        set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocol/${name}.xml")
        add_custom_command(
            OUTPUT "${protocol_out}/${name}-client-protocol.h"
            COMMAND ${WAYLAND_SCANNER} client-header "${xml}"
                    "${protocol_out}/${name}-client-protocol.h"
            COMMAND ${CMAKE_COMMAND}
                    -DFILE=${protocol_out}/${name}-client-protocol.h
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenameKeywordArgs.cmake
            DEPENDS "${xml}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenameKeywordArgs.cmake"
            VERBATIM)
        add_custom_command(
            OUTPUT "${protocol_out}/${name}-protocol.c"
            COMMAND ${WAYLAND_SCANNER} private-code "${xml}"
                    "${protocol_out}/${name}-protocol.c"
            DEPENDS "${xml}" VERBATIM)
        set(LIVEWALL_PROTOCOL_SOURCES
            ${LIVEWALL_PROTOCOL_SOURCES}
            "${protocol_out}/${name}-client-protocol.h"
            "${protocol_out}/${name}-protocol.c"
            PARENT_SCOPE)
    endfunction()

    livewall_wayland_protocol(wlr-layer-shell-unstable-v1)
    livewall_wayland_protocol(xdg-shell)
    livewall_wayland_protocol(xdg-output-unstable-v1)

    set(LIVEWALL_WAYLAND_SOURCES
        src/platform/wayland/WaylandBackend.cpp
        ${LIVEWALL_PROTOCOL_SOURCES})
endif()

# ---------------------------------------------------------------------------
# Core library
#
# Everything except the entry point, so the test executable links the same
# objects the app runs. The logic worth testing — coverage geometry, frame
# pacing, output sizing, index decoding — is the same four areas the macOS and
# Windows suites cover, and none of it needs a display or a decoder.
# ---------------------------------------------------------------------------
add_library(livewall_core STATIC
    src/support/Log.cpp
    src/support/Strings.cpp
    src/support/Json.cpp
    src/support/Guid.cpp
    src/support/Footprint.cpp
    src/support/Paths.cpp
    src/support/Settings.cpp
    src/support/Dynamic.cpp
    src/support/DBus.cpp
    src/support/DesktopVisibility.cpp
    src/support/PowerMonitor.cpp
    src/support/StartupItem.cpp

    src/platform/Backend.cpp
    src/platform/x11/X11Backend.cpp
    ${LIVEWALL_WAYLAND_SOURCES}

    src/render/FitMode.cpp
    src/render/EglDevice.cpp
    src/render/GlProgram.cpp
    src/render/GradientSource.cpp
    src/render/VideoSource.cpp

    src/import/FFmpeg.cpp
    src/import/FramePacer.cpp
    src/import/CodecSupport.cpp
    src/import/Transcoder.cpp
    src/import/Library.cpp

    src/app/OutputController.cpp
    src/app/WallpaperEngine.cpp
    src/app/ControlSocket.cpp
    src/app/TrayIcon.cpp
    src/app/AppHost.cpp
)

target_include_directories(livewall_core PUBLIC
    src
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_BINARY_DIR}/generated/protocol")

livewall_set_target_defaults(livewall_core)

target_link_libraries(livewall_core PUBLIC
    PkgConfig::X11
    PkgConfig::EGL
    Threads::Threads
    ${CMAKE_DL_LIBS}     # dlopen, for everything in the optional list
)

if(LIVEWALL_WAYLAND)
    target_link_libraries(livewall_core PUBLIC PkgConfig::WAYLAND)
    target_compile_definitions(livewall_core PUBLIC LIVEWALL_HAS_WAYLAND=1)
endif()

# Headers only. The libraries themselves are opened with dlopen at runtime and
# never appear on the link line; `pkg_check_modules` is used for the include
# path and nothing else.
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
pkg_check_modules(LIBVA REQUIRED libva libva-drm)
pkg_check_modules(DBUS REQUIRED dbus-1)
target_include_directories(livewall_core SYSTEM PUBLIC
    ${FFMPEG_INCLUDE_DIRS}
    ${LIBVA_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS})

livewall_embed_text(livewall_core
    OUTPUT "${CMAKE_BINARY_DIR}/generated/shaders"
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/wallpaper.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/gradient.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/external.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/rgb.frag")

# ---------------------------------------------------------------------------
# The app
#
# One binary, two jobs. With no arguments it is the daemon; with a subcommand it
# is the client that talks to a running daemon over the control socket. The
# macOS app has the status menu as its only interface and the Windows one has
# the tray; on Linux the tray is a nicety that half the desktops out there will
# not show, so the CLI is the interface and the tray is the nicety.
# ---------------------------------------------------------------------------
add_executable(LiveWall
    src/main.cpp
    src/app/Cli.cpp
)

set_target_properties(LiveWall PROPERTIES OUTPUT_NAME "livewall")
target_link_libraries(LiveWall PRIVATE livewall_core)
livewall_set_target_defaults(LiveWall)

include(GNUInstallDirs)
install(TARGETS LiveWall RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES packaging/livewall.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES ../../docs/icon.png
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        RENAME livewall.png)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(LIVEWALL_BUILD_TESTS "Build the LiveWall test suite" ON)

if(LIVEWALL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "LiveWall ${PROJECT_VERSION} — wayland: ${LIVEWALL_WAYLAND}, tests: ${LIVEWALL_BUILD_TESTS}")
