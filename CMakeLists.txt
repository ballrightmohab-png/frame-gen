cmake_minimum_required(VERSION 3.22)

project(LeviFrameGen)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    preloader_android
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG 0.2.2
)

FetchContent_MakeAvailable(preloader_android)

add_library(levi_framegen SHARED
    src/FrameGen.cpp
)

target_include_directories(levi_framegen PRIVATE
    ${preloader_android_SOURCE_DIR}/include
)

target_link_libraries(levi_framegen PRIVATE
    preloader
)

set_target_properties(levi_framegen PROPERTIES
    OUTPUT_NAME "levi_framegen"
)
