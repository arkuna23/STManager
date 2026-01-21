set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_minimum_required(VERSION 3.12)
project(STUtils VERSION 1.0.0 LANGUAGES CXX)

add_library(STUtils SHARED
    src/data.cpp
)

include(GenerateExportHeader)

set(STUTILS_EXPORT_H_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/generated/STUtils/stutils_export.h")

target_include_directories(STUtils PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>)

target_include_directories(STUtils PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# 3. 设置 C++ 标准
target_compile_features(STUtils PUBLIC cxx_std_11)
