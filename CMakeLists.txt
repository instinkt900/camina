cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_STR)
string(STRIP "${VERSION_STR}" PROJECT_VERSION_FULL)
# The CMake VERSION field takes a numeric x.y.z. Remove any pre-release suffix,
# for example -rc.1, before passing it on.
string(REGEX REPLACE "[-+].*$" "" PROJECT_VERSION "${PROJECT_VERSION_FULL}")

# The project is called Camina Engine. The C++ namespace stays "engine" on purpose.
# See DESIGN.md section 2.1 for the reasoning.
project(camina
    VERSION ${PROJECT_VERSION}
    DESCRIPTION "Camina Engine, a small 3D game engine"
    LANGUAGES CXX C
)

configure_file(
    src/core/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h
)

message(STATUS "Building Camina Engine ${PROJECT_VERSION_FULL} with CMake ${CMAKE_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(ClangTidy)
include(Docs)

if(CAMINA_DOCS_ONLY)
    set(CAMINA_BUILD_DOCS ON CACHE BOOL "Configure the Doxygen docs target" FORCE)
    camina_add_docs()
    return()
endif()

find_package(SDL3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(glm REQUIRED)
find_package(enkiTS REQUIRED)
find_package(Tracy REQUIRED)

add_subdirectory(src)
add_subdirectory(apps/runtime)

if(ENGINE_WITH_EDITOR)
    add_subdirectory(apps/editor)
endif()

enable_testing()
add_subdirectory(tests)

camina_add_docs()
