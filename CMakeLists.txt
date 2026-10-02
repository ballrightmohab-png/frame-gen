cmake_minimum_required(VERSION 3.22)
project(LeviFrameGen VERSION 2.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ANDROID)
    message(STATUS
        "Configuring LeviFrameGen in host verification mode. "
        "For Android arm64-v8a (.levipack), configure with the Android NDK toolchain.")
endif()

add_library(framegen SHARED
    src/FrameGenConfig.cpp
    src/CameraSmoother.cpp
    src/FramePacer.cpp
    src/GlFrameInterpolator.cpp
    src/HudRenderer.cpp
    src/FrameGenMod.cpp
)

target_include_directories(framegen PRIVATE
    src
    preloader_headers
)

# Build-time stub of libpreloader.so so libframegen.so records DT_NEEDED
# libpreloader.so and exact NDK/libc++ mangled undefined pl::* symbols.
# Do NOT ship libpreloader.so inside the .levipack; LeviLaunchroid injects
# its own runtime libpreloader.so into the game process.
add_library(preloader SHARED
    preloader_stub/preloader_stub.cpp
)
target_include_directories(preloader PRIVATE preloader_headers)
set_target_properties(preloader PROPERTIES
    OUTPUT_NAME "preloader"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stub"
)

if(ANDROID)
    target_link_libraries(framegen PRIVATE
        EGL
        GLESv2
        dl
        log
        preloader
    )
else()
    target_include_directories(preloader PRIVATE tests/host_stubs)
    target_include_directories(framegen PRIVATE tests/host_stubs)
    target_link_libraries(framegen PRIVATE
        dl
        preloader
    )

    enable_testing()
    add_executable(framegen_test_suite
        src/FrameGenConfig.cpp
        src/CameraSmoother.cpp
        src/FramePacer.cpp
        src/GlFrameInterpolator.cpp
        src/HudRenderer.cpp
        src/FrameGenMod.cpp
        tests/framegen_test_suite.cpp
    )
    target_include_directories(framegen_test_suite PRIVATE
        src
        preloader_headers
        tests/host_stubs
    )
    target_link_libraries(framegen_test_suite PRIVATE
        dl
        pthread
    )
    target_compile_options(framegen_test_suite PRIVATE
        -Wall
        -Wextra
        -Werror
        -O2
    )
    add_test(NAME FrameGenTestSuite COMMAND framegen_test_suite)
endif()

target_compile_options(framegen PRIVATE
    -Wall
    -Wextra
    -O2
    -ffunction-sections
    -fdata-sections
)

set_target_properties(framegen PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections"
    OUTPUT_NAME "framegen"
)
