cmake_minimum_required(VERSION 3.16)
project(piscan VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PISCAN_BUILD_TESTS "Build tests" ON)
option(PISCAN_WITH_MJPEG "Enable MJPEG decoding through libturbojpeg" ON)
option(PISCAN_NATIVE "Optimize for the build machine's CPU" OFF)

include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(PISCAN_BUILD_TESTS)
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

set(PISCAN_TURBOJPEG_FOUND OFF)
if(PISCAN_WITH_MJPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TURBOJPEG QUIET IMPORTED_TARGET libturbojpeg)
        if(TURBOJPEG_FOUND)
            set(PISCAN_TURBOJPEG_FOUND ON)
        endif()
    endif()
    if(NOT PISCAN_TURBOJPEG_FOUND)
        message(WARNING "libturbojpeg was not found; building without MJPEG support")
    endif()
endif()

set(PISCAN_CORE_SOURCES
    src/Bench.cpp
    src/Config.cpp
    src/DecodeFiles.cpp
    src/Decoder.cpp
    src/Dedupe.cpp
    src/FileFrameSource.cpp
    src/PwmBeeper.cpp
    src/GpioTrigger.cpp
    src/ImageFile.cpp
    src/Output.cpp
    src/Scanner.cpp
    src/V4l2Camera.cpp
)

add_library(piscan_core STATIC ${PISCAN_CORE_SOURCES})
target_compile_features(piscan_core PUBLIC cxx_std_20)
target_include_directories(piscan_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(piscan_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(piscan_core PUBLIC ZXing::ZXing Threads::Threads)

add_executable(piscan src/main.cpp)
target_compile_features(piscan PRIVATE cxx_std_20)
target_compile_definitions(piscan PRIVATE PISCAN_VERSION=\"${PROJECT_VERSION}\")
target_link_libraries(piscan PRIVATE piscan_core)

if(PISCAN_TURBOJPEG_FOUND)
    target_compile_definitions(piscan_core PUBLIC PISCAN_HAVE_TURBOJPEG)
    target_link_libraries(piscan_core PUBLIC PkgConfig::TURBOJPEG)
endif()

foreach(target piscan_core piscan)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(PISCAN_NATIVE)
        target_compile_options(${target} PRIVATE -mcpu=native)
    endif()
endforeach()

if(PISCAN_BUILD_TESTS)
    enable_testing()
    add_executable(piscan_tests tests/test_main.cpp)
    add_executable(piscan_perf_tests tests/test_perf.cpp)
    target_compile_features(piscan_perf_tests PRIVATE cxx_std_20)
    target_compile_options(piscan_perf_tests PRIVATE -Wall -Wextra)
    target_link_libraries(piscan_perf_tests PRIVATE piscan_core)
    if(PISCAN_NATIVE)
        target_compile_options(piscan_perf_tests PRIVATE -mcpu=native)
    endif()
    target_compile_features(piscan_tests PRIVATE cxx_std_20)
    target_compile_options(piscan_tests PRIVATE -Wall -Wextra)
    target_link_libraries(piscan_tests PRIVATE piscan_core)
    if(PISCAN_NATIVE)
        target_compile_options(piscan_tests PRIVATE -mcpu=native)
    endif()
    add_test(
        NAME piscan_tests
        COMMAND piscan_tests "$<TARGET_FILE:piscan>" "${CMAKE_CURRENT_BINARY_DIR}/bench-smoke.pgm"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
    add_test(
        NAME piscan_perf_tests
        COMMAND piscan_perf_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    )
endif()
