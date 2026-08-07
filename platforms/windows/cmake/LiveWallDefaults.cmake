# Compiler and preprocessor settings shared by every target.

function(livewall_set_target_defaults target)
    target_compile_definitions(${target} PUBLIC
        UNICODE
        _UNICODE
        # Trims perhaps 40% off the windows.h parse and, more usefully, keeps
        # the min/max macros from colliding with std::min/std::max.
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        NOSERVICE
        NOMCX
        NOIME
        # Windows 10 1809. Below this there is no
        # SetProcessDpiAwarenessContext, no IDCompositionDesktopDevice and no
        # MF_MPEG4SINK_MOOV_BEFORE_MDAT, all of which the app uses.
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000006
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            # C4100: unreferenced formal parameter. Window procedures and COM
            # callbacks have fixed signatures and routinely ignore arguments;
            # renaming them all to nothing would be noise.
            /wd4100
        )
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O2;/Oi;/GL;/Gy;/GS->
            $<$<CONFIG:Debug>:/Od;/Zi;/RTC1>
        )
        target_link_options(${target} PRIVATE
            # /OPT:REF and /OPT:ICF are what keep the binary in the same order
            # of magnitude as the 504 KB macOS one.
            $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>
            # The exploit-mitigation flags cost nothing at these sizes.
            /DYNAMICBASE
            /NXCOMPAT
            /HIGHENTROPYVA
        )

        # Static CRT: a tray app that a user copies to a folder and runs should
        # not fail on a machine without the redistributable installed. The
        # macOS app has no runtime to miss, and this is the closest equivalent.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
