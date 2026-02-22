cmake_minimum_required(VERSION 3.12)

project(STManager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(STManager SHARED
    src/data.cpp
)

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
