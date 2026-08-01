cmake_minimum_required(VERSION 3.25)

project(bt_download VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)
endif()

option(BT_DOWNLOAD_BUILD_TESTS "Build bt_download tests" ON)
option(BT_DOWNLOAD_BUILD_BENCHMARKS "Build bt_download performance benchmarks" ON)

find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(bt_download_core
    src/engine.cpp
    src/path_safety.cpp
    src/protocol.cpp
    src/task.cpp
)
target_include_directories(bt_download_core PUBLIC include)
target_compile_features(bt_download_core PUBLIC cxx_std_20)
target_link_libraries(bt_download_core
    PUBLIC
        LibtorrentRasterbar::torrent-rasterbar
        nlohmann_json::nlohmann_json
)
target_compile_definitions(bt_download_core PUBLIC
    BT_DOWNLOAD_VERSION="${PROJECT_VERSION}"
    BT_DOWNLOAD_PROTOCOL_VERSION="1.0"
    _WIN32_WINNT=0x0A00
)
if(MSVC)
    target_compile_options(bt_download_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(bt_download_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(bt_download src/main.cpp)
target_link_libraries(bt_download PRIVATE bt_download_core)

install(TARGETS bt_download RUNTIME DESTINATION .)

if(BT_DOWNLOAD_BUILD_BENCHMARKS AND WIN32)
    add_executable(bt_download_performance_benchmark
        tests/performance_benchmark.cpp
    )
    target_compile_features(bt_download_performance_benchmark PRIVATE cxx_std_20)
    target_link_libraries(bt_download_performance_benchmark
        PRIVATE
            LibtorrentRasterbar::torrent-rasterbar
            nlohmann_json::nlohmann_json
            psapi
            ws2_32
    )
    add_dependencies(bt_download_performance_benchmark bt_download)
    if(MSVC)
        target_compile_options(bt_download_performance_benchmark PRIVATE /W4 /permissive- /utf-8 /EHsc)
    else()
        target_compile_options(bt_download_performance_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BT_DOWNLOAD_BUILD_TESTS)
    enable_testing()
    add_executable(bt_download_tests
        tests/test_main.cpp
        tests/magnet_metadata_test.cpp
        tests/path_safety_test.cpp
        tests/protocol_test.cpp
        tests/resume_data_test.cpp
        tests/task_test.cpp
    )
    target_link_libraries(bt_download_tests PRIVATE bt_download_core)
    add_test(NAME bt_download_tests COMMAND bt_download_tests)

    add_executable(bt_download_integration_tests
        tests/download_integration_test.cpp
    )
    target_link_libraries(bt_download_integration_tests PRIVATE bt_download_core)
    if(WIN32)
        target_link_libraries(bt_download_integration_tests PRIVATE ws2_32)
    endif()
    add_test(NAME bt_download_integration_tests COMMAND bt_download_integration_tests)
    set_tests_properties(bt_download_integration_tests PROPERTIES
        LABELS integration
        TIMEOUT 90
    )
endif()
