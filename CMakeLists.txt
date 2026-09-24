cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)
endif()
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

project(bt_download VERSION 0.2.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

option(BT_DOWNLOAD_BUILD_TESTS "Build bt_download tests" ON)
option(BT_DOWNLOAD_BUILD_BENCHMARKS "Build bt_download performance benchmarks" ON)

# Full automatic inlining under MSVC 19.51 makes several libtorrent/nlohmann-
# heavy translation units terminate with FAST_FAIL_STACK_COOKIE_CHECK_FAILURE.
# Keep the normal Release optimization level for the project while limiting
# inlining for this toolset generation. Some translation units still trip the
# stack-cookie check with /Ob1, so the generation is compiled without inline
# expansion.
if(MSVC AND MSVC_VERSION EQUAL 1951)
    add_compile_options("$<$<CONFIG:Release>:/Ob0>")
endif()

find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(bt_download_core
    src/config.cpp
    src/engine.cpp
    src/error_mapping.cpp
    src/http_download.cpp
    src/path_safety.cpp
    src/proxy_config.cpp
    src/protocol.cpp
    src/seeding_policy.cpp
    src/task.cpp
    src/tracker_config.cpp
)
if(MSVC AND MSVC_VERSION EQUAL 1951)
    # The expanded engine implementation still triggers the compiler/runtime
    # stack-cookie failure at /O2. Keep the workaround local to that TU.
    set_source_files_properties(src/engine.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Release>:/O1>"
    )
endif()
target_include_directories(bt_download_core PUBLIC include)
target_compile_features(bt_download_core PUBLIC cxx_std_20)
target_link_libraries(bt_download_core
    PUBLIC
        CURL::libcurl
        LibtorrentRasterbar::torrent-rasterbar
        nlohmann_json::nlohmann_json
)
if(APPLE)
    target_sources(bt_download_core PRIVATE src/system_cost.mm)
    target_link_libraries(bt_download_core PRIVATE "-framework Foundation" "-framework Network")
else()
    target_sources(bt_download_core PRIVATE src/system_cost.cpp)
endif()
if(WIN32)
    target_link_libraries(bt_download_core PRIVATE ole32 uuid)
endif()
target_compile_definitions(bt_download_core PUBLIC
    BT_DOWNLOAD_VERSION="${PROJECT_VERSION}"
    BT_DOWNLOAD_PROTOCOL_VERSION="1.5"
    _WIN32_WINNT=0x0A00
)
if(MSVC)
    target_compile_options(bt_download_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(bt_download_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(bt_download src/main.cpp)
target_link_libraries(bt_download PRIVATE bt_download_core)

if(WIN32)
    install(TARGETS bt_download
        RUNTIME_DEPENDENCIES
            DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            PRE_EXCLUDE_REGEXES
                "api-ms-.*"
                "ext-ms-.*"
            POST_EXCLUDE_REGEXES
                ".*[\\\\/][Ss][Yy][Ss][Tt][Ee][Mm]32[\\\\/].*"
                ".*[\\\\/][Ss][Yy][Ss][Ww][Oo][Ww]64[\\\\/].*"
        RUNTIME DESTINATION .
    )
else()
    install(TARGETS bt_download RUNTIME DESTINATION .)
endif()

if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
endif()

if(WIN32 AND MSVC AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    configure_file(
        cmake/InstallReleaseMetadata.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallReleaseMetadata.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallReleaseMetadata.cmake")
elseif(WIN32)
    message(WARNING "vcpkg paths are unavailable; release license and SBOM generation is disabled")
endif()

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
        tests/config_test.cpp
        tests/file_priority_test.cpp
        tests/http_download_test.cpp
        tests/test_main.cpp
        tests/error_mapping_test.cpp
        tests/magnet_metadata_test.cpp
        tests/path_safety_test.cpp
        tests/proxy_config_test.cpp
        tests/protocol_test.cpp
        tests/resume_data_test.cpp
        tests/tabbed_details_test.cpp
        tests/task_test.cpp
        tests/user_agent_test.cpp
    )
    target_link_libraries(bt_download_tests PRIVATE bt_download_core)
    target_include_directories(bt_download_tests PRIVATE src)
    add_test(NAME bt_download_tests COMMAND bt_download_tests)

    add_test(NAME bt_download_process_smoke_test
        COMMAND ${CMAKE_COMMAND}
            "-DENGINE_PATH=$<TARGET_FILE:bt_download>"
            "-DSTATE_PATH=${CMAKE_CURRENT_BINARY_DIR}/process-smoke-state"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_smoke_test.cmake"
    )

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
