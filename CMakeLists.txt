cmake_minimum_required(VERSION 3.12)

project(STManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(STManager SHARED
    src/archive_stream.cpp
    src/data.cpp
    src/discovery_mdns.cpp
    src/git_manifest.cpp
    src/locate.cpp
    src/path_safety.cpp
    src/sync.cpp
    src/sync_server.cpp
    src/sync_protocol.cpp
    src/tcp_transport.cpp
    src/trusted_device_store.cpp
)

find_package(LibArchive REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

if(TARGET LibArchive::LibArchive)
    target_link_libraries(STManager PRIVATE LibArchive::LibArchive)
else()
    target_include_directories(STManager PRIVATE ${LibArchive_INCLUDE_DIRS})
    target_link_libraries(STManager PRIVATE ${LibArchive_LIBRARIES})
endif()

target_link_libraries(STManager PRIVATE nlohmann_json::nlohmann_json)

include(GenerateExportHeader)

set(STMANAGER_EXPORT_H_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/generated/STManager/stmanager_export.h"
)

get_filename_component(_export_dir "${STMANAGER_EXPORT_H_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_export_dir}")

generate_export_header(STManager
    EXPORT_FILE_NAME "${STMANAGER_EXPORT_H_PATH}"
)

target_include_directories(STManager
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(STManager PUBLIC cxx_std_11)

add_executable(stmanager
    cli/cli_args.cpp
    cli/cli_state.cpp
    cli/main.cpp
)

target_link_libraries(stmanager PRIVATE STManager)
target_compile_features(stmanager PRIVATE cxx_std_11)
target_include_directories(stmanager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli)

enable_testing()

add_executable(stmanager_tests
    tests/stmanager_tests.cpp
    tests/test_helpers.cpp
)

target_include_directories(stmanager_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_link_libraries(stmanager_tests
    PRIVATE
        STManager
        nlohmann_json::nlohmann_json
)

if(TARGET LibArchive::LibArchive)
    target_link_libraries(stmanager_tests PRIVATE LibArchive::LibArchive)
else()
    target_include_directories(stmanager_tests PRIVATE ${LibArchive_INCLUDE_DIRS})
    target_link_libraries(stmanager_tests PRIVATE ${LibArchive_LIBRARIES})
endif()

target_compile_features(stmanager_tests PRIVATE cxx_std_11)

add_test(NAME stmanager_tests COMMAND stmanager_tests)

add_executable(stmanager_cli_tests
    tests/cli_tests.cpp
    tests/test_helpers.cpp
    cli/cli_args.cpp
    cli/cli_state.cpp
)

target_include_directories(stmanager_cli_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/cli
)

target_link_libraries(stmanager_cli_tests
    PRIVATE
        STManager
)

target_compile_features(stmanager_cli_tests PRIVATE cxx_std_11)

add_test(NAME stmanager_cli_tests COMMAND stmanager_cli_tests)

add_executable(stmanager_sync_tests
    tests/sync_network_tests.cpp
    tests/test_helpers.cpp
)

target_include_directories(stmanager_sync_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/cli
)

target_link_libraries(stmanager_sync_tests
    PRIVATE
        STManager
        Threads::Threads
)

target_compile_features(stmanager_sync_tests PRIVATE cxx_std_11)

add_test(NAME stmanager_sync_tests COMMAND stmanager_sync_tests)
