cmake_minimum_required(VERSION 3.12)

project(STManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)

option(STMANAGER_CLI_STATIC_LINK "Build stmanager CLI with static linking" ON)
option(STMANAGER_CLI_SIZE_OPTIMIZED_RELEASE "Optimize stmanager size in Release builds" ON)
option(STMANAGER_LIBRARY_SIZE_OPTIMIZED "Optimize STManager library size in Release builds" ON)
option(STMANAGER_BUILD_CLI "Build stmanager CLI binary" ON)
option(STMANAGER_BUILD_TESTS "Build STManager test targets" ON)
option(STMANAGER_BUILD_SHARED "Build shared STManager library target" ON)

if(ANDROID)
    set(STMANAGER_BUILD_CLI OFF CACHE BOOL "Build stmanager CLI binary" FORCE)
    set(STMANAGER_BUILD_TESTS OFF CACHE BOOL "Build STManager test targets" FORCE)
endif()

set(STMANAGER_SOURCES
    src/archive_stream.cpp
    src/data.cpp
    src/discovery_mdns.cpp
    src/fs_ops.cpp
    src/git_manifest.cpp
    src/locate.cpp
    src/path_safety.cpp
    src/platform_compat.cpp
    src/sync.cpp
    src/sync_server.cpp
    src/sync_protocol.cpp
    src/tcp_transport.cpp
    src/trusted_device_store.cpp
)

if(STMANAGER_BUILD_SHARED)
    add_library(STManager SHARED
        ${STMANAGER_SOURCES}
    )
endif()

add_library(STManagerStatic STATIC
    ${STMANAGER_SOURCES}
)

if(ANDROID)
    set(STMANAGER_ANDROID_16KB_PAGE_LINK_OPTION "-Wl,-z,max-page-size=16384")
    set(_stmanager_android_has_16kb_page_flag FALSE)
    foreach(_stmanager_linker_flags_var
            CMAKE_EXE_LINKER_FLAGS
            CMAKE_SHARED_LINKER_FLAGS
            CMAKE_MODULE_LINKER_FLAGS)
        if("${${_stmanager_linker_flags_var}}" MATCHES "max-page-size=16384")
            set(_stmanager_android_has_16kb_page_flag TRUE)
        endif()
    endforeach()

    if(NOT _stmanager_android_has_16kb_page_flag)
        if(STMANAGER_BUILD_SHARED)
            target_link_options(STManager PRIVATE "${STMANAGER_ANDROID_16KB_PAGE_LINK_OPTION}")
        endif()
        target_link_options(STManagerStatic INTERFACE "${STMANAGER_ANDROID_16KB_PAGE_LINK_OPTION}")
    endif()
endif()

find_package(LibArchive REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

if(TARGET LibArchive::LibArchive)
    if(STMANAGER_BUILD_SHARED)
        target_link_libraries(STManager PRIVATE LibArchive::LibArchive)
    endif()
    target_link_libraries(STManagerStatic PRIVATE LibArchive::LibArchive)
else()
    if(STMANAGER_BUILD_SHARED)
        target_include_directories(STManager PRIVATE ${LibArchive_INCLUDE_DIRS})
    endif()
    target_include_directories(STManagerStatic PRIVATE ${LibArchive_INCLUDE_DIRS})
    if(STMANAGER_BUILD_SHARED)
        target_link_libraries(STManager PRIVATE ${LibArchive_LIBRARIES})
    endif()
    target_link_libraries(STManagerStatic PRIVATE ${LibArchive_LIBRARIES})
endif()

if(STMANAGER_BUILD_SHARED)
    target_link_libraries(STManager PRIVATE nlohmann_json::nlohmann_json)
endif()
target_link_libraries(STManagerStatic PRIVATE nlohmann_json::nlohmann_json)

if(WIN32)
    if(STMANAGER_BUILD_SHARED)
        target_link_libraries(STManager PRIVATE ws2_32)
    endif()
    target_link_libraries(STManagerStatic PUBLIC ws2_32)
endif()

include(GenerateExportHeader)

set(STMANAGER_EXPORT_H_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/generated/STManager/stmanager_export.h"
)

get_filename_component(_export_dir "${STMANAGER_EXPORT_H_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_export_dir}")

if(STMANAGER_BUILD_SHARED)
    generate_export_header(STManager
        EXPORT_FILE_NAME "${STMANAGER_EXPORT_H_PATH}"
    )
else()
    file(WRITE "${STMANAGER_EXPORT_H_PATH}" "#ifndef STMANAGER_EXPORT_H\n#define STMANAGER_EXPORT_H\n#define STMANAGER_EXPORT\n#define STMANAGER_NO_EXPORT\n#define STMANAGER_DEPRECATED\n#define STMANAGER_DEPRECATED_EXPORT STMANAGER_EXPORT\n#define STMANAGER_DEPRECATED_NO_EXPORT STMANAGER_NO_EXPORT\n#endif\n")
endif()

if(STMANAGER_BUILD_SHARED)
    target_include_directories(STManager
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
            $<INSTALL_INTERFACE:include>
    )
endif()
target_include_directories(STManagerStatic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

if(STMANAGER_BUILD_SHARED)
    target_compile_features(STManager PUBLIC cxx_std_11)
endif()
target_compile_features(STManagerStatic PUBLIC cxx_std_11)
target_compile_definitions(STManagerStatic PUBLIC STMANAGER_STATIC_DEFINE)

if(STMANAGER_LIBRARY_SIZE_OPTIMIZED AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(STMANAGER_BUILD_SHARED)
        target_compile_options(STManager PRIVATE
            $<$<CONFIG:Release>:-Os>
            $<$<CONFIG:MinSizeRel>:-Os>
            $<$<CONFIG:Release>:-g0>
            $<$<CONFIG:MinSizeRel>:-g0>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:MinSizeRel>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
            $<$<CONFIG:MinSizeRel>:-fdata-sections>
        )

        target_link_options(STManager PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:MinSizeRel>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,--exclude-libs,ALL>
            $<$<CONFIG:MinSizeRel>:-Wl,--exclude-libs,ALL>
            $<$<CONFIG:Release>:-Wl,--strip-all>
            $<$<CONFIG:MinSizeRel>:-Wl,--strip-all>
        )
    endif()

    target_compile_options(STManagerStatic PRIVATE
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:MinSizeRel>:-Os>
        $<$<CONFIG:Release>:-g0>
        $<$<CONFIG:MinSizeRel>:-g0>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:MinSizeRel>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
        $<$<CONFIG:MinSizeRel>:-fdata-sections>
    )
endif()

set(STMANAGER_RUNTIME_LINK_TARGET STManagerStatic)
if(STMANAGER_BUILD_SHARED)
    set(STMANAGER_RUNTIME_LINK_TARGET STManager)
endif()

set(STMANAGER_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifact/stmanager")
set(STMANAGER_BUNDLE_INCLUDE_DIR "${STMANAGER_BUNDLE_DIR}/include/STManager")
set(STMANAGER_BUNDLE_LIB_DIR "${STMANAGER_BUNDLE_DIR}/lib")

if(STMANAGER_BUILD_SHARED)
    add_custom_target(stmanager_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${STMANAGER_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STMANAGER_BUNDLE_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STMANAGER_BUNDLE_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/include/STManager"
            "${STMANAGER_BUNDLE_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STMANAGER_EXPORT_H_PATH}"
            "${STMANAGER_BUNDLE_INCLUDE_DIR}/stmanager_export.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:STManagerStatic>"
            "${STMANAGER_BUNDLE_LIB_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:STManager>"
            "${STMANAGER_BUNDLE_LIB_DIR}/"
        DEPENDS STManagerStatic STManager
        COMMENT "Bundling STManager libraries and headers"
    )
else()
    add_custom_target(stmanager_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory "${STMANAGER_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STMANAGER_BUNDLE_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STMANAGER_BUNDLE_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/include/STManager"
            "${STMANAGER_BUNDLE_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STMANAGER_EXPORT_H_PATH}"
            "${STMANAGER_BUNDLE_INCLUDE_DIR}/stmanager_export.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:STManagerStatic>"
            "${STMANAGER_BUNDLE_LIB_DIR}/"
        DEPENDS STManagerStatic
        COMMENT "Bundling STManager libraries and headers"
    )
endif()

if(STMANAGER_BUILD_CLI)
    add_executable(stmanager
        cli/cli_args.cpp
        cli/cli_command_selector.cpp
        cli/cli_net.cpp
        cli/cli_pair.cpp
        cli/cli_state.cpp
        cli/main.cpp
    )

    set(STMANAGER_CLI_STATIC_LINK_ACTIVE OFF)
    if(STMANAGER_CLI_STATIC_LINK AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set(STMANAGER_CLI_STATIC_LINK_ACTIVE ON)
    endif()

    if(STMANAGER_CLI_STATIC_LINK_ACTIVE OR NOT STMANAGER_BUILD_SHARED)
        target_link_libraries(stmanager PRIVATE STManagerStatic)
        target_compile_definitions(stmanager PRIVATE STMANAGER_STATIC_DEFINE)
    else()
        target_link_libraries(stmanager PRIVATE ${STMANAGER_RUNTIME_LINK_TARGET})
    endif()
    target_compile_features(stmanager PRIVATE cxx_std_11)
    target_include_directories(stmanager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli)

    if(STMANAGER_CLI_STATIC_LINK_ACTIVE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            if((UNIX AND NOT APPLE) OR MINGW)
                target_link_options(stmanager PRIVATE
                    $<$<CONFIG:Release>:-static>
                    $<$<CONFIG:Release>:-static-libgcc>
                    $<$<CONFIG:Release>:-static-libstdc++>
                )
            endif()
        endif()
    endif()

    if(STMANAGER_CLI_SIZE_OPTIMIZED_RELEASE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(stmanager PRIVATE
                $<$<CONFIG:Release>:-Os>
                $<$<CONFIG:Release>:-ffunction-sections>
                $<$<CONFIG:Release>:-fdata-sections>
            )
            target_link_options(stmanager PRIVATE
                $<$<CONFIG:Release>:-Wl,--gc-sections>
                $<$<CONFIG:Release>:-Wl,--strip-all>
            )
        endif()
    endif()
endif()

if(STMANAGER_BUILD_TESTS)
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
            ${STMANAGER_RUNTIME_LINK_TARGET}
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
        cli/cli_command_selector.cpp
        cli/cli_net.cpp
        cli/cli_pair.cpp
        cli/cli_state.cpp
    )

    target_include_directories(stmanager_cli_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/cli
    )

    target_link_libraries(stmanager_cli_tests
        PRIVATE
            ${STMANAGER_RUNTIME_LINK_TARGET}
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
            ${STMANAGER_RUNTIME_LINK_TARGET}
            Threads::Threads
    )

    target_compile_features(stmanager_sync_tests PRIVATE cxx_std_11)

    add_test(NAME stmanager_sync_tests COMMAND stmanager_sync_tests)
endif()
