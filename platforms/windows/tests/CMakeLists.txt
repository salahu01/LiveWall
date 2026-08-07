add_executable(livewall_tests
    TestMain.cpp
    CoverageTests.cpp
    PacerTests.cpp
    OutputSizeTests.cpp
    IndexTests.cpp
)

target_link_libraries(livewall_tests PRIVATE livewall_core)
target_include_directories(livewall_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
livewall_set_target_defaults(livewall_tests)

set_target_properties(livewall_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)

# One CTest entry for the whole binary rather than one per case. The harness
# prints each case's name and result, and --output-on-failure surfaces that,
# so splitting them would add CMake noise without adding information.
add_test(NAME livewall COMMAND livewall_tests)
