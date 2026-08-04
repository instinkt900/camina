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
find_package(volk REQUIRED)
find_package(VulkanMemoryAllocator REQUIRED)
find_package(imgui REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(EnTT REQUIRED)
find_package(stb REQUIRED)
find_package(cgltf REQUIRED)
find_package(meshoptimizer REQUIRED)

# The package exports one target and names it after the static library, so both
# the package name and the target read "spirv-reflect-static". Only the cooker
# links it. See tools/cooker/CMakeLists.txt.
find_package(spirv-reflect-static REQUIRED)

# The ImGui backends live in the Conan package, so this needs find_package(imgui)
# above and no Conan install in the docs job.
include(ImGuiBackends)

# Every executable lands in one directory, and the cooked content sits inside
# it. platform::cooked_content_root() looks next to the running executable, so
# putting them together is what makes that call find anything.
#
# This also means one cook serves the runtime, the editor, and the tests. The
# presets use single-config Ninja on both platforms. A multi-config generator
# would need the per-config form of this variable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(ENGINE_COOKED_CONTENT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/content")

# The vendored submodules. Rule 4.4 keeps this list short. Only the cooker
# links anything from here.
add_subdirectory(third_party)

add_subdirectory(src)

# The cooker comes after src, because it links engine::core. It looks for
# glslc and fails when it is missing, so it must come after the docs-only
# return above. The docs job has no Conan and no glslc.
add_subdirectory(tools/cooker)

# The game module comes before the applications, because both link it. Rule 4.3.
add_subdirectory(sandbox)

add_subdirectory(apps/runtime)

if(ENGINE_WITH_EDITOR)
    add_subdirectory(apps/editor)
endif()

enable_testing()
add_subdirectory(tests)

camina_add_docs()
