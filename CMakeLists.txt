cmake_minimum_required(VERSION 3.16)
project(piscanner VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PISCANNER_BUILD_TESTS "Build tests" ON)
option(PISCANNER_WITH_MJPEG "Enable MJPEG decoding through libturbojpeg" ON)
option(PISCANNER_NATIVE "Optimize for the build machine's CPU" OFF)

include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(PISCANNER_BUILD_TESTS)
    set(ZXING_WRITERS NEW CACHE STRING "" FORCE)
else()
    set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
    zxing_cpp
    GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
    GIT_TAG v3.1.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zxing_cpp)

find_package(Threads REQUIRED)

set(PISCANNER_TURBOJPEG_FOUND OFF)
if(PISCANNER_WITH_MJPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TURBOJPEG QUIET IMPORTED_TARGET libturbojpeg)
        if(TURBOJPEG_FOUND)
            set(PISCANNER_TURBOJPEG_FOUND ON)
        endif()
    endif()
    if(NOT PISCANNER_TURBOJPEG_FOUND)
        message(WARNING "libturbojpeg was not found; building without MJPEG support")
    endif()
endif()

set(PISCANNER_CORE_SOURCES
    src/Bench.cpp
    src/Capture.cpp
    src/Config.cpp
    src/DecodeFiles.cpp
    src/Decoder.cpp
    src/Dedupe.cpp
    src/DisplaySink.cpp
    src/FileFrameSource.cpp
    src/PwmBeeper.cpp
    src/GpioTrigger.cpp
    src/ImageFile.cpp
    src/Log.cpp
    src/Output.cpp
    src/Scanner.cpp
    src/V4l2Camera.cpp
)

add_library(piscanner_core STATIC ${PISCANNER_CORE_SOURCES})
target_compile_features(piscanner_core PUBLIC cxx_std_20)
target_include_directories(piscanner_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(piscanner_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(piscanner_core PUBLIC ZXing::ZXing Threads::Threads)

add_executable(piscanner src/main.cpp)
target_compile_features(piscanner PRIVATE cxx_std_20)
target_compile_definitions(piscanner PRIVATE PISCANNER_VERSION=\"${PROJECT_VERSION}\")
target_link_libraries(piscanner PRIVATE piscanner_core)

if(PISCANNER_TURBOJPEG_FOUND)
    target_compile_definitions(piscanner_core PUBLIC PISCANNER_HAVE_TURBOJPEG)
    target_link_libraries(piscanner_core PUBLIC PkgConfig::TURBOJPEG)
endif()

foreach(target piscanner_core piscanner)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(PISCANNER_NATIVE)
        target_compile_options(${target} PRIVATE -mcpu=native)
    endif()
endforeach()

if(PISCANNER_BUILD_TESTS)
    enable_testing()
    add_executable(piscanner_tests tests/test_main.cpp)
    add_executable(piscanner_perf_tests tests/test_perf.cpp)
    target_compile_features(piscanner_perf_tests PRIVATE cxx_std_20)
    target_compile_options(piscanner_perf_tests PRIVATE -Wall -Wextra)
    target_link_libraries(piscanner_perf_tests PRIVATE piscanner_core)
    if(PISCANNER_NATIVE)
        target_compile_options(piscanner_perf_tests PRIVATE -mcpu=native)
    endif()
    target_compile_features(piscanner_tests PRIVATE cxx_std_20)
    target_compile_options(piscanner_tests PRIVATE -Wall -Wextra)
    target_link_libraries(piscanner_tests PRIVATE piscanner_core)
    if(PISCANNER_NATIVE)
        target_compile_options(piscanner_tests PRIVATE -mcpu=native)
    endif()
    add_test(
        NAME piscanner_tests
        COMMAND piscanner_tests "$<TARGET_FILE:piscanner>" "${CMAKE_CURRENT_BINARY_DIR}/bench-smoke.pgm"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
    add_test(
        NAME piscanner_perf_tests
        COMMAND piscanner_perf_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
endif()
