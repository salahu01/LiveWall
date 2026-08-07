# The suite links livewall_core, so it exercises the same objects the app runs
# rather than a copy compiled with different flags.
#
# Everything in here is pure logic — coverage geometry, frame pacing, output
# sizing, index decoding — so it needs no display, no GPU, no decoder and no
# media files, and runs on a headless CI runner. What it cannot cover is the
# X11 stacking, the layer-shell surface and the VA-API path, all of which need a
# real session; those are listed under the README's known limits rather than
# faked here.

add_executable(livewall_tests
    main.cpp
    CoverageTests.cpp
    PacingTests.cpp
    OutputSizeTests.cpp
    IndexTests.cpp
)

target_include_directories(livewall_tests PRIVATE .)
target_link_libraries(livewall_tests PRIVATE livewall_core)
livewall_set_target_defaults(livewall_tests)

# One ctest entry per area, so a failure names the thing that broke rather than
# "the test binary".
foreach(suite coverage pacing paced_fps output_size fit presets index json guid strings)
    add_test(NAME ${suite} COMMAND livewall_tests ${suite})
endforeach()
